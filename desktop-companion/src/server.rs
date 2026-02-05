use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::Result;
use tokio::net::UdpSocket;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, error, info, warn};

use crate::config::Config;
use crate::protocol::{
    parse_packet, DiscoveryRequest, DiscoveryResponse, GameInfo, Heartbeat, IconChunk, IconEnd,
    IconRequest, MessageType, StatsResponse, StatsUpload,
};

/// Maximum UDP packet size
const MAX_PACKET_SIZE: usize = 2048;

/// Event from the server to main loop
#[derive(Debug, Clone)]
pub enum ServerEvent {
    /// PSP connected
    PspConnected {
        addr: SocketAddr,
        name: String,
        battery: u8,
    },
    /// PSP disconnected (timeout)
    PspDisconnected { addr: SocketAddr, name: String },
    /// Game info updated
    GameInfoUpdated { addr: SocketAddr, info: GameInfo },
    /// Heartbeat received
    HeartbeatReceived {
        #[allow(dead_code)]
        addr: SocketAddr,
        heartbeat: Heartbeat,
    },
    /// Icon received completely
    IconReceived {
        game_id: String,
        data: Vec<u8>,
    },
    /// Stats requested by PSP
    StatsRequested {
        addr: SocketAddr,
        psp_name: String,
    },
    /// Stats uploaded from PSP
    StatsUploaded {
        addr: SocketAddr,
        psp_name: String,
        last_updated: u64,
        json_data: String,
    },
}

/// Connected PSP state
struct PspConnection {
    name: String,
    last_seen: Instant,
    current_game: Option<GameInfo>,
    icon_buffer: HashMap<String, IconBuffer>,
    discovery_sent: bool,
    persistent: bool,
    /// Accumulating stats upload data
    stats_upload_buffer: Option<StatsUploadBuffer>,
}

/// Buffer for accumulating chunked stats uploads
struct StatsUploadBuffer {
    chunks: Vec<Option<Vec<u8>>>,
    total_chunks: u16,
    received_chunks: u16,
    last_updated: u64,
}

/// Buffer for receiving icon chunks
struct IconBuffer {
    chunks: Vec<Option<Vec<u8>>>,
    total_chunks: u16,
    received_chunks: u16,
}

/// Commands that can be sent to the server
#[derive(Debug, Clone)]
pub enum ServerCommand {
    /// Request an icon for a game from a specific PSP
    RequestIcon { addr: SocketAddr, game_id: String },
    /// Send stats response to a PSP
    SendStats { 
        addr: SocketAddr, 
        json_data: Vec<u8>,
        last_updated: u64,
    },
}

/// UDP server for receiving PSP data
pub struct Server {
    config: Config,
    socket: Option<Arc<UdpSocket>>,
    discovery_socket: Option<Arc<UdpSocket>>,
    local_ipv4: Option<Ipv4Addr>,
    connections: Arc<RwLock<HashMap<SocketAddr, PspConnection>>>,
    event_tx: mpsc::Sender<ServerEvent>,
    command_rx: Option<mpsc::Receiver<ServerCommand>>,
}

impl Server {
    pub fn new(config: Config, event_tx: mpsc::Sender<ServerEvent>) -> Self {
        Self {
            config,
            socket: None,
            discovery_socket: None,
            local_ipv4: None,
            connections: Arc::new(RwLock::new(HashMap::new())),
            event_tx,
            command_rx: None,
        }
    }

    /// Create a command channel for the server
    pub fn create_command_channel(&mut self) -> mpsc::Sender<ServerCommand> {
        let (tx, rx) = mpsc::channel(32);
        self.command_rx = Some(rx);
        tx
    }

    fn get_local_ipv4() -> Option<Ipv4Addr> {
        let sock = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
        sock.connect("8.8.8.8:80").ok()?;
        match sock.local_addr().ok()?.ip() {
            IpAddr::V4(ip) => Some(ip),
            _ => None,
        }
    }

    #[allow(dead_code)]
    fn guess_broadcast(ip: Ipv4Addr) -> Ipv4Addr {
        let mut octets = ip.octets();
        octets[3] = 255;
        Ipv4Addr::from(octets)
    }

    /// Start the server
    pub async fn start(&mut self) -> Result<()> {
        // Bind main data socket
        let addr = format!("0.0.0.0:{}", self.config.network.listen_port);
        let socket = UdpSocket::bind(&addr).await?;
        socket.set_broadcast(true)?;
        info!("Listening for PSP connections on {}", addr);
        self.local_ipv4 = Self::get_local_ipv4();
        if let Some(ip) = self.local_ipv4 {
            info!("LAN IP: {}", ip);
        }
        self.socket = Some(Arc::new(socket));

        // Bind discovery socket if enabled
        if self.config.network.auto_discovery {
            let discovery_addr = format!("0.0.0.0:{}", self.config.network.discovery_port);
            match UdpSocket::bind(&discovery_addr).await {
                Ok(sock) => {
                    sock.set_broadcast(true)?;
                    info!("Discovery listening on {}", discovery_addr);
                    self.discovery_socket = Some(Arc::new(sock));
                }
                Err(e) => {
                    warn!("Failed to bind discovery socket: {}", e);
                }
            }
        }

        Ok(())
    }

    /// Run the main receive loop
    pub async fn run(&mut self) -> Result<()> {
        let socket = self.socket.as_ref().ok_or_else(|| anyhow::anyhow!("Server not started"))?.clone();
        let connections = self.connections.clone();
        let event_tx = self.event_tx.clone();
        let timeout_secs = self.config.network.timeout_seconds;

        // Spawn timeout checker
        let connections_clone = connections.clone();
        let event_tx_clone = event_tx.clone();
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(10));
            loop {
                interval.tick().await;
                let mut conns = connections_clone.write().await;
                let now = Instant::now();
                let mut to_remove = Vec::new();

                for (addr, conn) in conns.iter() {
                    // Skip timeout for persistent connections (send_once mode)
                    if conn.persistent {
                        continue;
                    }
                    if now.duration_since(conn.last_seen).as_secs() > timeout_secs {
                        to_remove.push(*addr);
                    }
                }

                for addr in to_remove {
                    if let Some(conn) = conns.remove(&addr) {
                        info!("PSP {} ({}) disconnected (timeout)", conn.name, addr);
                        let _ = event_tx_clone.send(ServerEvent::PspDisconnected { 
                            addr, 
                            name: conn.name 
                        }).await;
                    }
                }
            }
        });

        // Take the command receiver if we have one
        let mut command_rx = self.command_rx.take();

        // Main receive loop
        let mut buf = [0u8; MAX_PACKET_SIZE];
        loop {
            tokio::select! {
                // Handle incoming packets
                result = socket.recv_from(&mut buf) => {
                    match result {
                        Ok((len, addr)) => {
                            if let Err(e) = self.handle_packet(&buf[..len], addr).await {
                                debug!("Error handling packet from {}: {}", addr, e);
                            }
                        }
                        Err(e) => {
                            error!("Error receiving packet: {}", e);
                        }
                    }
                }
                // Handle commands from main loop
                Some(cmd) = async { 
                    match &mut command_rx {
                        Some(rx) => rx.recv().await,
                        None => std::future::pending().await,
                    }
                } => {
                    match cmd {
                        ServerCommand::RequestIcon { addr, game_id } => {
                            if let Err(e) = self.request_icon(addr, &game_id).await {
                                warn!("Failed to request icon: {}", e);
                            }
                        }
                        ServerCommand::SendStats { addr, json_data, last_updated } => {
                            if let Err(e) = self.send_stats_response(addr, &json_data, last_updated).await {
                                warn!("Failed to send stats to {}: {}", addr, e);
                            }
                        }
                    }
                }
            }
        }
    }

    /// Handle an incoming packet
    async fn handle_packet(&self, data: &[u8], addr: SocketAddr) -> Result<()> {
        let (msg_type, payload) = parse_packet(data)?;

        match msg_type {
            MessageType::Heartbeat => {
                let heartbeat = Heartbeat::decode(payload)?;
                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }
                let should_send = self.update_last_seen(addr).await;
                if should_send {
                    if let Err(e) = self.send_discovery_request(addr).await {
                        warn!("Failed to send discovery request to {}: {}", addr, e);
                    }
                }
                self.event_tx
                    .send(ServerEvent::HeartbeatReceived { addr, heartbeat })
                    .await?;
            }

            MessageType::GameInfo => {
                let info = GameInfo::decode(payload)?;
                debug!("Game info from {}: {} - {}", addr, info.game_id, info.title);
                if info.has_icon {
                    debug!("Game {} has icon data", info.game_id);
                }

                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }

                let should_send = self.update_last_seen(addr).await;
                if should_send {
                    if let Err(e) = self.send_discovery_request(addr).await {
                        warn!("Failed to send discovery request to {}: {}", addr, e);
                    }
                }

                // Update connection state
                {
                    let mut conns = self.connections.write().await;
                    if let Some(conn) = conns.get_mut(&addr) {
                        conn.last_seen = Instant::now();
                        conn.current_game = Some(info.clone());
                        // Update connection name from GameInfo if we have a better name
                        if !info.psp_name.is_empty() && conn.name != info.psp_name {
                            debug!("Updating connection name from '{}' to '{}'", conn.name, info.psp_name);
                            conn.name = info.psp_name.clone();
                        }
                        // If PSP sent persistent flag (send_once mode), mark connection
                        if info.persistent {
                            conn.persistent = true;
                            info!("PSP {} marked as persistent (send_once mode)", conn.name);
                        }
                    }
                }

                self.event_tx
                    .send(ServerEvent::GameInfoUpdated { addr, info })
                    .await?;
            }

            MessageType::IconChunk => {
                let chunk = IconChunk::decode(payload)?;
                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }
                self.handle_icon_chunk(addr, chunk).await?;
            }

            MessageType::IconEnd => {
                let end = IconEnd::decode(payload)?;
                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }
                self.handle_icon_end(addr, end).await?;
            }

            MessageType::DiscoveryResponse => {
                let response = DiscoveryResponse::decode(payload)?;
                info!(
                    "Discovered PSP: {} (v{}, battery: {}%)",
                    response.psp_name, response.version, response.battery_percent
                );

                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }

                // Add to connections if new
                {
                    let mut conns = self.connections.write().await;
                    if !conns.contains_key(&addr) {
                        conns.insert(
                            addr,
                            PspConnection {
                                name: response.psp_name.clone(),
                                last_seen: Instant::now(),
                                current_game: None,
                                icon_buffer: HashMap::new(),
                                discovery_sent: true,
                                persistent: false,
                                stats_upload_buffer: None,
                            },
                        );
                    }
                }

                self.event_tx
                    .send(ServerEvent::PspConnected {
                        addr,
                        name: response.psp_name,
                        battery: response.battery_percent,
                    })
                    .await?;
            }

            MessageType::StatsRequest => {
                info!("Stats request received from {}", addr);
                
                // Send ACK first
                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }
                
                // Get PSP name for the event
                let psp_name = {
                    let conns = self.connections.read().await;
                    conns.get(&addr).map(|c| c.name.clone()).unwrap_or_default()
                };
                
                // Emit event to main loop to handle stats request
                self.event_tx
                    .send(ServerEvent::StatsRequested { addr, psp_name })
                    .await?;
            }

            MessageType::StatsUpload => {
                let upload = StatsUpload::decode(payload)?;
                debug!("Stats upload chunk {}/{} from {}", upload.chunk_index + 1, upload.total_chunks, addr);
                
                // Send ACK
                if let Err(e) = self.send_ack(addr).await {
                    warn!("Failed to send ACK to {}: {}", addr, e);
                }
                
                // Handle the chunk and check if complete
                self.handle_stats_upload_chunk(addr, upload).await?;
            }

            _ => {
                debug!("Unhandled message type: {:?}", msg_type);
            }
        }

        Ok(())
    }

    /// Update last seen time for a connection
    async fn update_last_seen(&self, addr: SocketAddr) -> bool {
        let mut conns = self.connections.write().await;
        let auto = self.config.network.auto_discovery;
        if let Some(conn) = conns.get_mut(&addr) {
            conn.last_seen = Instant::now();
            if auto && !conn.discovery_sent {
                conn.discovery_sent = true;
                return true;
            }
            return false;
        }

        // New connection without discovery
        conns.insert(
            addr,
            PspConnection {
                name: format!("PSP-{}", addr.port()),
                last_seen: Instant::now(),
                current_game: None,
                icon_buffer: HashMap::new(),
                discovery_sent: auto,
                persistent: false,
                stats_upload_buffer: None,
            },
        );
        auto
    }

    async fn send_discovery_request(&self, addr: SocketAddr) -> Result<()> {
        if !self.config.network.auto_discovery {
            return Ok(());
        }

        if let Some(socket) = &self.socket {
            let request = DiscoveryRequest::new(self.config.network.listen_port);
            let packet = request.encode();
            let target = SocketAddr::new(addr.ip(), self.config.network.discovery_port);
            socket.send_to(&packet, target).await?;
            info!("Sent discovery request to {}", target);
        }

        Ok(())
    }

    async fn send_ack(&self, addr: SocketAddr) -> Result<()> {
        if let Some(socket) = &self.socket {
            let mut buf = Vec::with_capacity(5);
            buf.extend_from_slice(crate::protocol::MAGIC);
            buf.push(crate::protocol::MessageType::Ack as u8);
            socket.send_to(&buf, addr).await?;
        }
        Ok(())
    }

    /// Request an icon from a connected PSP
    pub async fn request_icon(&self, addr: SocketAddr, game_id: &str) -> Result<()> {
        if let Some(socket) = &self.socket {
            let request = IconRequest::new(game_id);
            let packet = request.encode();
            socket.send_to(&packet, addr).await?;
            info!("Requested icon for {} from {}", game_id, addr);
        }
        Ok(())
    }

    /// Send stats response to PSP (chunked if necessary)
    pub async fn send_stats_response(&self, addr: SocketAddr, json_data: &[u8], last_updated: u64) -> Result<()> {
        if let Some(socket) = &self.socket {
            let chunks = StatsResponse::new_chunks(json_data, last_updated);
            info!("Sending {} stats chunks ({} bytes) to {}", chunks.len(), json_data.len(), addr);
            
            for chunk in chunks {
                let packet = chunk.encode();
                socket.send_to(&packet, addr).await?;
                // Small delay between chunks to avoid overwhelming PSP
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        }
        Ok(())
    }

    /// Handle a stats upload chunk
    async fn handle_stats_upload_chunk(&self, addr: SocketAddr, upload: StatsUpload) -> Result<()> {
        let (psp_name, completed_data) = {
            let mut conns = self.connections.write().await;
            if let Some(conn) = conns.get_mut(&addr) {
                // Initialize buffer on first chunk
                if upload.chunk_index == 0 {
                    conn.stats_upload_buffer = Some(StatsUploadBuffer {
                        chunks: vec![None; upload.total_chunks as usize],
                        total_chunks: upload.total_chunks,
                        received_chunks: 0,
                        last_updated: upload.last_updated,
                    });
                }
                
                // Store chunk
                if let Some(buffer) = &mut conn.stats_upload_buffer {
                    if (upload.chunk_index as usize) < buffer.chunks.len() {
                        if buffer.chunks[upload.chunk_index as usize].is_none() {
                            buffer.received_chunks += 1;
                        }
                        buffer.chunks[upload.chunk_index as usize] = Some(upload.json_data);
                        
                        // Check if complete
                        if buffer.received_chunks >= buffer.total_chunks {
                            // Assemble all chunks
                            let mut data = Vec::new();
                            for chunk in buffer.chunks.iter().flatten() {
                                data.extend(chunk);
                            }
                            let last_updated = buffer.last_updated;
                            conn.stats_upload_buffer = None;
                            (conn.name.clone(), Some((data, last_updated)))
                        } else {
                            (String::new(), None)
                        }
                    } else {
                        (String::new(), None)
                    }
                } else {
                    (String::new(), None)
                }
            } else {
                (String::new(), None)
            }
        };
        
        // If we have completed data, emit event
        if let Some((data, last_updated)) = completed_data {
            let json_data = String::from_utf8_lossy(&data).to_string();
            info!("Received complete stats upload from {} ({} bytes)", psp_name, json_data.len());
            self.event_tx
                .send(ServerEvent::StatsUploaded { 
                    addr, 
                    psp_name,
                    last_updated,
                    json_data,
                })
                .await?;
        }
        
        Ok(())
    }

    /// Handle an icon chunk
    async fn handle_icon_chunk(&self, addr: SocketAddr, chunk: IconChunk) -> Result<()> {
        let mut conns = self.connections.write().await;
        if let Some(conn) = conns.get_mut(&addr) {
            let buffer = conn
                .icon_buffer
                .entry(chunk.game_id.clone())
                .or_insert_with(|| IconBuffer {
                    chunks: vec![None; chunk.total_chunks as usize],
                    total_chunks: chunk.total_chunks,
                    received_chunks: 0,
                });

            if (chunk.chunk_index as usize) < buffer.chunks.len() {
                if buffer.chunks[chunk.chunk_index as usize].is_none() {
                    buffer.received_chunks += 1;
                }
                buffer.chunks[chunk.chunk_index as usize] = Some(chunk.data);
            }
        }
        Ok(())
    }

    /// Handle icon end marker
    async fn handle_icon_end(&self, addr: SocketAddr, end: IconEnd) -> Result<()> {
        let icon_data = {
            let mut conns = self.connections.write().await;
            if let Some(conn) = conns.get_mut(&addr) {
                if let Some(buffer) = conn.icon_buffer.remove(&end.game_id) {
                    if buffer.received_chunks != buffer.total_chunks {
                        warn!(
                            "Icon chunks missing for {}: {}/{}",
                            end.game_id, buffer.received_chunks, buffer.total_chunks
                        );
                    }
                    // Assemble chunks
                    let mut data = Vec::with_capacity(end.total_size as usize);
                    for chunk in buffer.chunks.into_iter().flatten() {
                        data.extend(chunk);
                    }

                    // Verify CRC
                    let crc = crc32fast::hash(&data);
                    if crc == end.crc32 {
                        Some(data)
                    } else {
                        warn!(
                            "Icon CRC mismatch for {}: expected {:08x}, got {:08x}",
                            end.game_id, end.crc32, crc
                        );
                        None
                    }
                } else {
                    None
                }
            } else {
                None
            }
        };

        if let Some(data) = icon_data {
            info!("Received complete icon for {} ({} bytes)", end.game_id, data.len());
            self.event_tx
                .send(ServerEvent::IconReceived {
                    game_id: end.game_id,
                    data,
                })
                .await?;
        }

        Ok(())
    }

    /// Send discovery broadcast
    #[allow(dead_code)]
    pub async fn send_discovery_broadcast(&self) -> Result<()> {
        if let Some(socket) = &self.socket {
            let request = DiscoveryRequest::new(self.config.network.listen_port);
            let packet = request.encode();

            let mut targets = Vec::new();
            let broadcast_addr: SocketAddr = format!("255.255.255.255:{}", self.config.network.discovery_port)
                .parse()?;
            targets.push(broadcast_addr);

            if let Some(ip) = self.local_ipv4 {
                let guess = Self::guess_broadcast(ip);
                let guess_addr: SocketAddr = format!("{}:{}", guess, self.config.network.discovery_port).parse()?;
                if guess_addr != broadcast_addr {
                    targets.push(guess_addr);
                }
            }

            for target in targets {
                socket.send_to(&packet, target).await?;
                debug!("Sent discovery broadcast to {}", target);
            }
        }
        Ok(())
    }
}
