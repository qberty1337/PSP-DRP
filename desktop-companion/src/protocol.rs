//! Protocol definitions matching the PSP plugin
//! See shared/protocol.md for full documentation

use std::io;

/// Magic bytes at the start of every packet
pub const MAGIC: &[u8; 4] = b"PSPR";

/// Protocol version
pub const VERSION: &str = "1.0.0";

/// Message types
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageType {
    // PSP -> Desktop
    Heartbeat = 0x01,
    GameInfo = 0x02,
    IconChunk = 0x03,
    IconEnd = 0x04,
    StatsRequest = 0x05,  // Request usage stats from desktop
    DiscoveryResponse = 0x21,

    // Desktop -> PSP
    Ack = 0x10,
    IconRequest = 0x11,  // Request icon for a game_id
    StatsResponse = 0x12,  // Send usage stats to PSP
    DiscoveryRequest = 0x20,
}

impl TryFrom<u8> for MessageType {
    type Error = ();

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0x01 => Ok(Self::Heartbeat),
            0x02 => Ok(Self::GameInfo),
            0x03 => Ok(Self::IconChunk),
            0x04 => Ok(Self::IconEnd),
            0x05 => Ok(Self::StatsRequest),
            0x10 => Ok(Self::Ack),
            0x11 => Ok(Self::IconRequest),
            0x12 => Ok(Self::StatsResponse),
            0x20 => Ok(Self::DiscoveryRequest),
            0x21 => Ok(Self::DiscoveryResponse),
            _ => Err(()),
        }
    }
}

/// PSP state/activity type
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PspState {
    #[default]
    Xmb = 0,
    Game = 1,
    Homebrew = 2,
    Video = 3,
    Music = 4,
}

impl TryFrom<u8> for PspState {
    type Error = ();

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Xmb),
            1 => Ok(Self::Game),
            2 => Ok(Self::Homebrew),
            3 => Ok(Self::Video),
            4 => Ok(Self::Music),
            _ => Err(()),
        }
    }
}

impl PspState {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Xmb => "Browsing XMB",
            Self::Game => "Playing",
            Self::Homebrew => "Running Homebrew",
            Self::Video => "Watching Video",
            Self::Music => "Listening to Music",
        }
    }
}

/// Heartbeat message from PSP
#[derive(Debug, Clone)]
pub struct Heartbeat {
    #[allow(dead_code)]
    pub uptime_seconds: u32,
    pub wifi_strength: u8,
}

impl Heartbeat {
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 5 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Heartbeat too short",
            ));
        }

        Ok(Self {
            uptime_seconds: u32::from_le_bytes([data[0], data[1], data[2], data[3]]),
            wifi_strength: data[4],
        })
    }
}

/// Game information from PSP
#[derive(Debug, Clone)]
pub struct GameInfo {
    pub game_id: String,
    pub title: String,
    #[allow(dead_code)]
    pub start_time: u32,
    pub state: PspState,
    pub has_icon: bool,
    pub persistent: bool,
    pub psp_name: String,
}

impl GameInfo {
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 144 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "GameInfo too short",
            ));
        }

        // Read game_id (10 bytes, null-terminated)
        let game_id = read_string(&data[0..10]);

        // Read title (128 bytes, null-terminated)
        let title = read_string(&data[10..138]);

        // Read start_time (4 bytes)
        let start_time = u32::from_le_bytes([data[138], data[139], data[140], data[141]]);

        // Read state (1 byte)
        let state = PspState::try_from(data[142]).unwrap_or_default();

        // Read has_icon (1 byte)
        let has_icon = data[143] != 0;

        // Read persistent (1 byte) - for send_once mode
        let persistent = if data.len() > 144 { data[144] != 0 } else { false };

        // Read psp_name (32 bytes starting at offset 145)
        let psp_name = if data.len() >= 177 {
            read_string(&data[145..177])
        } else {
            String::new()
        };

        Ok(Self {
            game_id,
            title,
            start_time,
            state,
            has_icon,
            persistent,
            psp_name,
        })
    }
}

/// Icon chunk from PSP
#[derive(Debug, Clone)]
pub struct IconChunk {
    pub game_id: String,
    pub chunk_index: u16,
    pub total_chunks: u16,
    pub data: Vec<u8>,
}

impl IconChunk {
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 16 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "IconChunk too short",
            ));
        }

        let game_id = read_string(&data[0..10]);
        let chunk_index = u16::from_le_bytes([data[10], data[11]]);
        let total_chunks = u16::from_le_bytes([data[12], data[13]]);
        let data_length = u16::from_le_bytes([data[14], data[15]]) as usize;

        if data.len() < 16 + data_length {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "IconChunk data incomplete",
            ));
        }

        Ok(Self {
            game_id,
            chunk_index,
            total_chunks,
            data: data[16..16 + data_length].to_vec(),
        })
    }
}

/// Icon end marker from PSP
#[derive(Debug, Clone)]
pub struct IconEnd {
    pub game_id: String,
    pub total_size: u32,
    pub crc32: u32,
}

impl IconEnd {
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 18 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "IconEnd too short",
            ));
        }

        let game_id = read_string(&data[0..10]);
        let total_size = u32::from_le_bytes([data[10], data[11], data[12], data[13]]);
        let crc32 = u32::from_le_bytes([data[14], data[15], data[16], data[17]]);

        Ok(Self {
            game_id,
            total_size,
            crc32,
        })
    }
}

/// Discovery response from PSP
#[derive(Debug, Clone)]
pub struct DiscoveryResponse {
    pub psp_name: String,
    pub version: String,
    pub battery_percent: u8,
}

impl DiscoveryResponse {
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 41 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "DiscoveryResponse too short",
            ));
        }

        let psp_name = read_string(&data[0..32]);
        let version = read_string(&data[32..40]);
        let battery_percent = data[40];

        Ok(Self {
            psp_name,
            version,
            battery_percent,
        })
    }
}

/// Discovery request (sent by desktop)
#[derive(Debug, Clone)]
pub struct DiscoveryRequest {
    pub listen_port: u16,
    pub version: String,
}

impl DiscoveryRequest {
    pub fn new(listen_port: u16) -> Self {
        Self {
            listen_port,
            version: VERSION.to_string(),
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(4 + 1 + 10);

        // Magic
        buf.extend_from_slice(MAGIC);

        // Type
        buf.push(MessageType::DiscoveryRequest as u8);

        // Listen port
        buf.extend_from_slice(&self.listen_port.to_le_bytes());

        // Version (8 bytes, null-padded)
        let version_bytes = self.version.as_bytes();
        buf.extend_from_slice(&version_bytes[..version_bytes.len().min(7)]);
        buf.resize(buf.len() + (8 - version_bytes.len().min(7)), 0);

        buf
    }
}

/// Icon request (sent by desktop to request icon for a game)
#[derive(Debug, Clone)]
pub struct IconRequest {
    pub game_id: String,
}

impl IconRequest {
    pub fn new(game_id: &str) -> Self {
        Self {
            game_id: game_id.to_string(),
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(4 + 1 + 10);

        // Magic
        buf.extend_from_slice(MAGIC);

        // Type
        buf.push(MessageType::IconRequest as u8);

        // Game ID (10 bytes, null-padded)
        let game_id_bytes = self.game_id.as_bytes();
        let copy_len = game_id_bytes.len().min(9);
        buf.extend_from_slice(&game_id_bytes[..copy_len]);
        buf.resize(buf.len() + (10 - copy_len), 0);

        buf
    }
}

/// Stats response (sent by desktop to PSP with usage stats)
/// Format: [total_games:u16][total_playtime:u64][chunk_index:u16][total_chunks:u16][data_len:u16][json_data...]
#[derive(Debug, Clone)]
pub struct StatsResponse {
    pub total_games: u16,
    pub total_playtime: u64,  // Total playtime in seconds
    pub chunk_index: u16,
    pub total_chunks: u16,
    pub json_data: Vec<u8>,   // Chunk of JSON usage data
}

impl StatsResponse {
    /// Create a new stats response with chunked data
    pub fn new_chunks(json_data: &[u8], total_games: u16, total_playtime: u64) -> Vec<Self> {
        const MAX_CHUNK_SIZE: usize = 1024;  // Keep packets under UDP MTU
        
        let total_chunks = ((json_data.len() + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE).max(1) as u16;
        
        let mut chunks = Vec::new();
        let mut offset = 0;
        
        for chunk_index in 0..total_chunks {
            let end = (offset + MAX_CHUNK_SIZE).min(json_data.len());
            let chunk_data = if offset < json_data.len() {
                json_data[offset..end].to_vec()
            } else {
                Vec::new()
            };
            
            chunks.push(Self {
                total_games,
                total_playtime,
                chunk_index,
                total_chunks,
                json_data: chunk_data,
            });
            
            offset += MAX_CHUNK_SIZE;
        }
        
        chunks
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(4 + 1 + 16 + self.json_data.len());

        // Magic
        buf.extend_from_slice(MAGIC);

        // Type
        buf.push(MessageType::StatsResponse as u8);

        // Total games (2 bytes)
        buf.extend_from_slice(&self.total_games.to_le_bytes());
        
        // Total playtime (8 bytes)
        buf.extend_from_slice(&self.total_playtime.to_le_bytes());
        
        // Chunk index (2 bytes)
        buf.extend_from_slice(&self.chunk_index.to_le_bytes());
        
        // Total chunks (2 bytes)
        buf.extend_from_slice(&self.total_chunks.to_le_bytes());
        
        // Data length (2 bytes)
        buf.extend_from_slice(&(self.json_data.len() as u16).to_le_bytes());
        
        // JSON data
        buf.extend_from_slice(&self.json_data);

        buf
    }
}

/// Parse a packet and return the message type and payload
pub fn parse_packet(data: &[u8]) -> io::Result<(MessageType, &[u8])> {
    if data.len() < 5 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Packet too short",
        ));
    }

    // Check magic
    if &data[0..4] != MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Invalid magic bytes",
        ));
    }

    // Get message type
    let msg_type = MessageType::try_from(data[4])
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "Unknown message type"))?;

    Ok((msg_type, &data[5..]))
}

/// Helper to read a null-terminated string from fixed-size buffer
fn read_string(data: &[u8]) -> String {
    let end = data.iter().position(|&b| b == 0).unwrap_or(data.len());
    String::from_utf8_lossy(&data[..end]).to_string()
}
