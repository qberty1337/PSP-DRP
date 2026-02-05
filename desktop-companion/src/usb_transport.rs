//! USB Transport Module
//!
//! Implements USBHostFS-style communication with the PSP using rusb.
//! The PSP acts as a custom USB device with bulk endpoints for
//! bidirectional data transfer.

use std::time::Duration;

use anyhow::{Context, Result};
use rusb::{DeviceHandle, GlobalContext, UsbContext};
use tokio::sync::mpsc;
use tracing::{debug, error, info, warn};

use crate::config::UsbConfig;

/// USB Vendor/Product IDs - Sony PSP Type B (compatible with USBHostFS driver)
pub const USB_VENDOR_ID: u16 = 0x054C;   // Sony
pub const USB_PRODUCT_ID: u16 = 0x01C9;  // PSP Type B (driver PID becomes USB PID)

/// USB Endpoints
const EP_BULK_IN: u8 = 0x81;   // PSP -> PC
const EP_BULK_OUT: u8 = 0x02; // PC -> PSP

/// USB Interface number
const USB_INTERFACE: u8 = 0;

/// Transfer timeout (short to avoid blocking the async runtime)
const USB_TIMEOUT: Duration = Duration::from_millis(100);

/// Packet magic header for PSP DRP protocol
const PACKET_MAGIC: u32 = 0x50535044; // "PSPD"

/// Maximum packet size for USB transfers
const MAX_PACKET_SIZE: usize = 512;

/// Packet types matching PSP side
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PacketType {
    Heartbeat = 0x01,
    GameInfo = 0x02,
    Icon = 0x03,
    StatsRequest = 0x05,
    StatsUpload = 0x06,
    Ack = 0x10,
    IconRequest = 0x11,
    StatsResponse = 0x12,
}

/// USB Transport events sent to the main application
#[derive(Debug, Clone)]
pub enum UsbEvent {
    /// PSP device connected
    Connected,
    /// PSP device disconnected
    Disconnected,
    /// Received game info from PSP
    GameInfo(GameInfoPacket),
    /// Received icon data from PSP (complete)
    IconData { game_id: String, data: Vec<u8> },
    /// PSP requested stats sync
    StatsRequested { psp_name: String, local_timestamp: u64 },
    /// PSP uploaded stats data
    StatsUploaded { last_updated: u64, json_data: String },
    /// Error occurred
    Error(String),
}

/// Internal icon chunk for accumulation
#[derive(Debug, Clone)]
pub struct IconChunk {
    pub game_id: String,
    pub total_size: usize,
    pub chunk_offset: usize,
    pub chunk_data: Vec<u8>,
    pub chunk_num: u8,
    pub total_chunks: u8,
}

/// USB Transport commands from main application
#[derive(Debug, Clone)]
pub enum UsbCommand {
    /// Request icon for a game
    RequestIcon { game_id: String },
    /// Send stats response to PSP
    SendStatsResponse { last_updated: u64, json_data: String },
}

/// Game info packet received from PSP
#[derive(Debug, Clone)]
pub struct GameInfoPacket {
    pub game_id: String,
    pub title: String,
    pub state: u8,
    pub has_icon: bool,
    pub start_time: u64,
    pub persistent: bool,
    pub psp_name: String,
}

/// USB Transport handle for communicating with PSP
pub struct UsbTransport {
    handle: DeviceHandle<GlobalContext>,
    config: UsbConfig,
}

impl UsbTransport {
    /// Find and open the PSP DRP USB device
    pub fn find_device() -> Result<Option<Self>> {
        let context = GlobalContext::default();
        
        for device in context.devices()?.iter() {
            let desc = device.device_descriptor()?;
            
            if desc.vendor_id() == USB_VENDOR_ID && desc.product_id() == USB_PRODUCT_ID {
                info!(
                    "Found PSP DRP device: VID={:04x} PID={:04x}",
                    desc.vendor_id(),
                    desc.product_id()
                );
                
                let handle = device.open().context("Failed to open USB device")?;
                
                // Claim the interface
                #[cfg(target_os = "linux")]
                {
                    if handle.kernel_driver_active(USB_INTERFACE)? {
                        handle.detach_kernel_driver(USB_INTERFACE)?;
                    }
                }
                
                // Set configuration first (required for proper enumeration)
                if let Err(e) = handle.set_active_configuration(1) {
                    // May fail if already configured, that's OK
                    debug!("set_active_configuration: {} (may be expected)", e);
                }
                
                handle.claim_interface(USB_INTERFACE)
                    .context("Failed to claim USB interface")?;
                
                info!("PSP DRP USB device opened successfully");
                
                return Ok(Some(Self {
                    handle,
                    config: UsbConfig::default(),
                }));
            }
        }
        
        Ok(None)
    }
    
    /// Send data to PSP via bulk OUT endpoint
    pub fn send(&self, data: &[u8]) -> Result<usize> {
        let written = self.handle
            .write_bulk(EP_BULK_OUT, data, USB_TIMEOUT)
            .context("USB bulk write failed")?;
        
        debug!("USB: Sent {} bytes", written);
        Ok(written)
    }
    
    /// Receive data from PSP via bulk IN endpoint
    /// Returns Ok(Some(len)) on success, Ok(None) on timeout, Err on real error
    pub fn recv_with_timeout(&self, buffer: &mut [u8]) -> Result<Option<usize>> {
        match self.handle.read_bulk(EP_BULK_IN, buffer, USB_TIMEOUT) {
            Ok(len) => {
                if len > 0 {
                    debug!("USB: Received {} bytes", len);
                }
                Ok(Some(len))
            }
            Err(rusb::Error::Timeout) => {
                // Timeout is expected when no data is available - not an error
                Ok(None)
            }
            Err(e) => {
                // Other errors indicate actual problems
                Err(anyhow::anyhow!("USB bulk read failed: {}", e))
            }
        }
    }
    
    /// Send ACK packet to PSP
    pub fn send_ack(&self) -> Result<()> {
        let mut packet = [0u8; 8];
        
        // Magic header
        packet[0..4].copy_from_slice(&PACKET_MAGIC.to_le_bytes());
        // Packet type
        packet[4] = PacketType::Ack as u8;
        // Reserved
        packet[5..8].fill(0);
        
        self.send(&packet)?;
        Ok(())
    }
    
    /// Send icon request to PSP
    pub fn request_icon(&self, game_id: &str) -> Result<()> {
        // PSP expects: 8-byte header + 10-byte game_id + 6-byte padding = 24 bytes
        let mut packet = [0u8; 24];
        
        // Magic header
        packet[0..4].copy_from_slice(&PACKET_MAGIC.to_le_bytes());
        // Packet type
        packet[4] = PacketType::IconRequest as u8;
        // Reserved
        packet[5..8].fill(0);
        // Game ID (10 chars, null-padded)
        let game_id_bytes = game_id.as_bytes();
        let len = game_id_bytes.len().min(10);
        packet[8..8 + len].copy_from_slice(&game_id_bytes[..len]);
        // Remaining bytes (18-24) are padding, already zeroed
        
        self.send(&packet)?;
        Ok(())
    }
    
    /// Send stats response to PSP (chunked with ACK flow control)
    pub fn send_stats_response(&self, last_updated: u64, json_data: &str) -> Result<()> {
        const CHUNK_SIZE: usize = 480;
        let data = json_data.as_bytes();
        let total_bytes = data.len() as u32;
        let total_chunks = (data.len() + CHUNK_SIZE - 1) / CHUNK_SIZE;
        
        for (chunk_idx, chunk) in data.chunks(CHUNK_SIZE).enumerate() {
            // UsbStatsResponsePacket layout:
            // Header (8 bytes): magic(4), type(1), reserved(1), length(2)
            // last_updated(8), total_bytes(4), chunk_index(2), total_chunks(2), data_length(2), data[480]
            let mut packet = [0u8; 512];
            
            // Header
            packet[0..4].copy_from_slice(&PACKET_MAGIC.to_le_bytes());
            packet[4] = PacketType::StatsResponse as u8;
            packet[5] = 0; // reserved
            packet[6..8].copy_from_slice(&(504u16).to_le_bytes()); // payload length
            
            // Payload
            packet[8..16].copy_from_slice(&last_updated.to_le_bytes());
            packet[16..20].copy_from_slice(&total_bytes.to_le_bytes()); // total_bytes for verification
            packet[20..22].copy_from_slice(&(chunk_idx as u16).to_le_bytes());
            packet[22..24].copy_from_slice(&(total_chunks as u16).to_le_bytes());
            packet[24..26].copy_from_slice(&(chunk.len() as u16).to_le_bytes());
            packet[26..26 + chunk.len()].copy_from_slice(chunk);
            
            self.send(&packet)?;
            
            // Wait for ACK from PSP before sending next chunk
            let mut ack_buf = [0u8; 64];
            let start = std::time::Instant::now();
            loop {
                if let Ok(Some(size)) = self.recv_with_timeout(&mut ack_buf) {
                    if size >= 8 {
                        let magic = u32::from_le_bytes([ack_buf[0], ack_buf[1], ack_buf[2], ack_buf[3]]);
                        let pkt_type = ack_buf[4];
                        if magic == PACKET_MAGIC && pkt_type == PacketType::Ack as u8 {
                            debug!("USB: Got ACK for chunk {}/{}", chunk_idx + 1, total_chunks);
                            break;
                        }
                    }
                }
                // Timeout after 2 seconds
                if start.elapsed() > Duration::from_secs(2) {
                    warn!("USB: ACK timeout for chunk {}, continuing anyway", chunk_idx);
                    break;
                }
                // Small sleep to avoid busy-loop
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        
        info!("USB: Sent stats response ({} bytes, {} chunks)", data.len(), total_chunks);
        Ok(())
    }
    
    /// Parse a received packet
    pub fn parse_packet(&self, data: &[u8]) -> Result<Option<UsbEvent>> {
        if data.len() < 8 {
            return Ok(None);
        }
        
        // Check magic
        let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        if magic != PACKET_MAGIC {
            warn!("USB: Invalid packet magic: {:08X}", magic);
            return Ok(None);
        }
        
        let packet_type = data[4];
        
        match packet_type {
            0x01 => {
                // Heartbeat
                debug!("USB: Received heartbeat");
                Ok(None)
            }
            0x02 => {
                // Game info packet:
                // Header (8 bytes): magic(4), type(1), reserved(1), length(2)
                // Payload: game_id[10], title[64], state(1), has_icon(1), start_time(4), persistent(1), psp_name[32], padding[7]
                if data.len() < 128 {
                    return Ok(None);
                }
                
                // Parse game info packet matching PSP struct layout
                let game_id = extract_string(&data[8..18]);       // bytes 8-17 (10 chars)
                let title = extract_string(&data[18..82]);        // bytes 18-81 (64 chars)
                let state = data[82];                             // byte 82
                let has_icon = data[83] != 0;                     // byte 83
                let start_time = u32::from_le_bytes([data[84], data[85], data[86], data[87]]) as u64;  // bytes 84-87
                let persistent = data[88] != 0;                   // byte 88
                let psp_name = extract_string(&data[89..121]);    // bytes 89-120 (32 chars)
                
                info!("USB: Received game info: {} - {}", game_id, title);
                
                Ok(Some(UsbEvent::GameInfo(GameInfoPacket {
                    game_id,
                    title,
                    state,
                    has_icon,
                    start_time,
                    persistent,
                    psp_name,
                })))
            }
            0x03 => {
                // Icon chunk packet:
                // Header (8 bytes): magic(4), type(1), reserved(1), length(2)
                // Payload: game_id[10], total_size(2), chunk_offset(2), chunk_size(2), chunk_num(1), total_chunks(1), data[450]
                if data.len() < 26 {
                    return Ok(None);
                }
                
                let game_id = extract_string(&data[8..18]);
                let total_size = u16::from_le_bytes([data[18], data[19]]) as usize;
                let chunk_offset = u16::from_le_bytes([data[20], data[21]]) as usize;
                let chunk_size = u16::from_le_bytes([data[22], data[23]]) as usize;
                let chunk_num = data[24];
                let total_chunks = data[25];
                
                debug!("USB: Icon chunk {}/{} for {} (offset={}, size={}, total={})", 
                       chunk_num + 1, total_chunks, game_id, chunk_offset, chunk_size, total_size);
                
                // Return chunk info for accumulation
                if data.len() >= 26 + chunk_size {
                    let chunk_data = data[26..26 + chunk_size].to_vec();
                    
                    // If single chunk, emit the icon data directly
                    if total_chunks == 1 {
                        info!("USB: Received complete icon for {} ({} bytes)", game_id, chunk_size);
                        return Ok(Some(UsbEvent::IconData { 
                            game_id, 
                            data: chunk_data 
                        }));
                    } else {
                        // Multi-chunk icon - return chunk for accumulation via parse_icon_chunk
                        info!("USB: Received icon chunk {}/{} ({} bytes)", 
                              chunk_num + 1, total_chunks, chunk_size);
                        // Store in thread-local and assemble when complete
                        // For now, we'll handle this in the calling code
                    }
                }
                
                Ok(None)
            }
            0x05 => {
                // Stats request packet: header(8) + local_timestamp(8) = 16 bytes
                if data.len() < 16 {
                    return Ok(None);
                }
                
                let local_timestamp = u64::from_le_bytes([
                    data[8], data[9], data[10], data[11],
                    data[12], data[13], data[14], data[15]
                ]);
                
                info!("USB: Stats request received (timestamp={})", local_timestamp);
                
                // PSP name not included in this packet, use empty string
                Ok(Some(UsbEvent::StatsRequested {
                    psp_name: String::new(),
                    local_timestamp,
                }))
            }
            0x06 => {
                // Stats upload packet: header(8) + last_updated(8) + chunk_index(2) + total_chunks(2) + data_length(2) + data[480]
                if data.len() < 24 {
                    return Ok(None);
                }
                
                let last_updated = u64::from_le_bytes([
                    data[8], data[9], data[10], data[11],
                    data[12], data[13], data[14], data[15]
                ]);
                let chunk_index = u16::from_le_bytes([data[16], data[17]]);
                let total_chunks = u16::from_le_bytes([data[18], data[19]]);
                let data_length = u16::from_le_bytes([data[20], data[21]]) as usize;
                
                debug!("USB: Stats upload chunk {}/{} (len={})", 
                       chunk_index + 1, total_chunks, data_length);
                
                // For single chunk, emit directly
                if total_chunks == 1 && data.len() >= 22 + data_length {
                    let json_data = String::from_utf8_lossy(&data[22..22 + data_length]).to_string();
                    info!("USB: Stats upload complete ({} bytes)", data_length);
                    return Ok(Some(UsbEvent::StatsUploaded {
                        last_updated,
                        json_data,
                    }));
                }
                
                // Multi-chunk handling would go here (similar to icon chunking)
                // For now, we return None and let the calling code accumulate
                Ok(None)
            }
            _ => {
                warn!("USB: Unknown packet type: {:02X}", packet_type);
                Ok(None)
            }
        }
    }
    
    /// Close the USB transport
    pub fn close(self) {
        if let Err(e) = self.handle.release_interface(USB_INTERFACE) {
            warn!("Failed to release USB interface: {}", e);
        }
        info!("USB transport closed");
    }
    
    /// Try to parse an icon chunk from raw data (returns None if not an icon chunk)
    pub fn try_parse_icon_chunk(data: &[u8]) -> Option<IconChunk> {
        if data.len() < 26 {
            return None;
        }
        
        // Check magic
        let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        if magic != PACKET_MAGIC {
            return None;
        }
        
        // Check packet type (0x03 = icon chunk)
        if data[4] != 0x03 {
            return None;
        }
        
        let game_id = extract_string(&data[8..18]);
        let total_size = u16::from_le_bytes([data[18], data[19]]) as usize;
        let chunk_offset = u16::from_le_bytes([data[20], data[21]]) as usize;
        let chunk_size = u16::from_le_bytes([data[22], data[23]]) as usize;
        let chunk_num = data[24];
        let total_chunks = data[25];
        
        if data.len() < 26 + chunk_size {
            return None;
        }
        
        Some(IconChunk {
            game_id,
            total_size,
            chunk_offset,
            chunk_data: data[26..26 + chunk_size].to_vec(),
            chunk_num,
            total_chunks,
        })
    }
}

/// Extract a null-terminated string from bytes
fn extract_string(data: &[u8]) -> String {
    let end = data.iter().position(|&b| b == 0).unwrap_or(data.len());
    String::from_utf8_lossy(&data[..end]).to_string()
}
/// Spawn USB transport task
pub async fn spawn_usb_task(
    config: UsbConfig,
    event_tx: mpsc::Sender<UsbEvent>,
    mut command_rx: mpsc::Receiver<UsbCommand>,
) -> Result<tokio::task::JoinHandle<()>> {
    let handle = tokio::task::spawn(async move {
        info!("USB transport task started");
        
        let mut connected = false;
        let mut current_transport: Option<UsbTransport> = None;
        
        loop {
            // Try to find device if not connected
            if !connected {
                match UsbTransport::find_device() {
                    Ok(Some(transport)) => {
                        info!("PSP DRP USB device connected");
                        if let Err(e) = event_tx.send(UsbEvent::Connected).await {
                            error!("Failed to send connected event: {}", e);
                            break;
                        }
                        connected = true;
                        
                        // Send ACK
                        if let Err(e) = transport.send_ack() {
                            error!("Failed to send ACK: {}", e);
                        }
                        
                        current_transport = Some(transport);
                    }
                    Ok(None) => {
                        // No device found, wait and retry
                        tokio::time::sleep(config.poll_interval()).await;
                    }
                    Err(e) => {
                        warn!("USB device search error: {}", e);
                        tokio::time::sleep(config.poll_interval()).await;
                    }
                }
            }
            
            // Connected - run read/command loop
            if let Some(ref transport) = current_transport {
                let mut buffer = [0u8; MAX_PACKET_SIZE];
                // Icon chunk accumulator: game_id -> (total_size, chunks_received, data)
                let mut icon_buffer: std::collections::HashMap<String, (usize, u8, Vec<u8>)> = std::collections::HashMap::new();
                
                loop {
                    // Non-blocking check for commands
                    match command_rx.try_recv() {
                        Ok(cmd) => {
                            match cmd {
                                UsbCommand::RequestIcon { game_id } => {
                                    debug!("USB: Icon request command for {}", game_id);
                                    if let Err(e) = transport.request_icon(&game_id) {
                                        warn!("Failed to request icon: {}", e);
                                    }
                                }
                                UsbCommand::SendStatsResponse { last_updated, json_data } => {
                                    info!("USB: Sending stats response ({} bytes)", json_data.len());
                                    if let Err(e) = transport.send_stats_response(last_updated, &json_data) {
                                        warn!("Failed to send stats response: {}", e);
                                    }
                                }
                            }
                        }
                        Err(mpsc::error::TryRecvError::Empty) => {
                            // No commands, continue
                        }
                        Err(mpsc::error::TryRecvError::Disconnected) => {
                            info!("USB command channel closed");
                            break;
                        }
                    }
                    
                    // Try to read from USB
                    match transport.recv_with_timeout(&mut buffer) {
                        Ok(Some(len)) if len > 0 => {
                            debug!("USB: Received {} bytes", len);
                            
                            // First check if this is an icon chunk that needs accumulation
                            if let Some(chunk) = UsbTransport::try_parse_icon_chunk(&buffer[..len]) {
                                // Accumulate chunk
                                let entry = icon_buffer.entry(chunk.game_id.clone())
                                    .or_insert_with(|| (chunk.total_size, 0, vec![0u8; chunk.total_size]));
                                
                                // Copy chunk data to correct offset
                                if chunk.chunk_offset + chunk.chunk_data.len() <= entry.2.len() {
                                    entry.2[chunk.chunk_offset..chunk.chunk_offset + chunk.chunk_data.len()]
                                        .copy_from_slice(&chunk.chunk_data);
                                    entry.1 += 1;
                                    
                                    debug!("USB: Accumulated chunk {}/{} for {}", entry.1, chunk.total_chunks, chunk.game_id);
                                    
                                    // Check if complete
                                    if entry.1 >= chunk.total_chunks {
                                        info!("USB: Received complete icon for {} ({} bytes)", chunk.game_id, entry.0);
                                        let icon_data = entry.2.clone();
                                        let game_id = chunk.game_id.clone();
                                        icon_buffer.remove(&chunk.game_id);
                                        
                                        if let Err(e) = event_tx.send(UsbEvent::IconData { game_id, data: icon_data }).await {
                                            error!("Failed to send icon event: {}", e);
                                            break;
                                        }
                                    }
                                }
                                continue; // Don't process as regular packet
                            }
                            
                            if let Ok(Some(event)) = transport.parse_packet(&buffer[..len]) {
                                // Check if this is GameInfo with has_icon - request icon automatically
                                if let UsbEvent::GameInfo(ref game_info) = event {
                                    if game_info.has_icon && !game_info.game_id.is_empty() && game_info.game_id != "XMB" {
                                        debug!("USB: Game has icon, requesting it...");
                                        if let Err(e) = transport.request_icon(&game_info.game_id) {
                                            warn!("Failed to request icon: {}", e);
                                        }
                                    }
                                }
                                
                                if let Err(e) = event_tx.send(event).await {
                                    error!("Failed to send event: {}", e);
                                    break;
                                }
                            }
                        }
                        Ok(Some(_)) => {
                            // Empty read, continue
                        }
                        Ok(None) => {
                            // Timeout - no data available, but device still connected
                            // Yield to let other async tasks run
                            tokio::task::yield_now().await;
                        }
                        Err(e) => {
                            // Actual error - device disconnected or I/O failure
                            info!("USB read error: {} - reconnecting...", e);
                            let _ = event_tx.send(UsbEvent::Disconnected).await;
                            connected = false;
                            break;
                        }
                    }
                }
                
                if let Some(t) = current_transport.take() {
                    t.close();
                }
            }
            
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        
        info!("USB transport task ended");
    });
    
    Ok(handle)
}

