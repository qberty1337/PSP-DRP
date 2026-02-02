//! Configuration module for reading psp_drp.ini
//!
//! Reads the desktop companion IP address from the PSP DRP INI file
//! located at ms0:/seplugins/pspdrp/psp_drp.ini

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::c_void;

use psp::sys::{
    sceIoOpen, sceIoRead, sceIoWrite, sceIoClose, sceIoLseek,
    IoOpenFlags, IoWhence, SceUid,
};

/// Configuration values from psp_drp.ini
pub struct Config {
    /// Companion IP address [a, b, c, d]
    pub desktop_ip: [u8; 4],
    /// Port to connect on (default 9276)
    pub port: u16,
    /// Whether IP was found in config
    pub has_ip: bool,
    /// Offline mode - skip network, use local usage.json
    pub offline_mode: bool,
    /// Enable debug logging
    pub enable_logging: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            desktop_ip: [192, 168, 1, 100],  // Default fallback
            port: 9276,
            has_ip: false,
            offline_mode: false,
            enable_logging: false,
        }
    }
}

/// INI file path on PSP memory stick
const INI_PATH: &[u8] = b"ms0:/seplugins/pspdrp/psp_drp.ini\0";

/// Usage JSON file path on PSP memory stick
const USAGE_PATH: &[u8] = b"ms0:/seplugins/pspdrp/usage.json\0";

/// Read and parse the config file
pub fn load_config() -> Config {
    let mut config = Config::default();
    
    // Try to read INI file
    let content = match read_file(INI_PATH) {
        Some(bytes) => bytes,
        None => return config,
    };
    
    // Parse as string
    let text = match core::str::from_utf8(&content) {
        Ok(s) => s,
        Err(_) => return config,
    };
    
    // Parse each line
    for line in text.lines() {
        let line = line.trim();
        
        // Skip comments and empty lines
        if line.is_empty() || line.starts_with(';') {
            continue;
        }
        
        // Parse key = value
        if let Some(eq_pos) = line.find('=') {
            let key = line[..eq_pos].trim();
            let value = line[eq_pos + 1..].trim();
            
            match key {
                "desktop_ip" => {
                    if let Some(ip) = parse_ip(value) {
                        config.desktop_ip = ip;
                        config.has_ip = true;
                    }
                }
                "port" => {
                    if let Ok(p) = value.parse::<u16>() {
                        config.port = p;
                    }
                }
                "offline_mode" => {
                    config.offline_mode = value == "1" || value.eq_ignore_ascii_case("true");
                }
                "enable_logging" => {
                    config.enable_logging = value == "1" || value.eq_ignore_ascii_case("true");
                }
                _ => {}
            }
        }
    }
    
    config
}

/// Load usage.json from the pspdrp folder
/// Returns the JSON string if file exists and is readable
pub fn load_usage_json() -> Option<String> {
    let bytes = read_file(USAGE_PATH)?;
    String::from_utf8(bytes).ok()
}

/// Save usage.json to the pspdrp folder
/// Returns true if successful
pub fn save_usage_json(json: &[u8]) -> bool {
    write_file(USAGE_PATH, json)
}

/// Parse an IP address string like "192.168.1.100" into [u8; 4]
fn parse_ip(s: &str) -> Option<[u8; 4]> {
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    
    let mut parts = [0u8; 4];
    let mut part_idx = 0;
    let mut current_num: u16 = 0;
    let mut has_digits = false;
    
    for c in s.chars() {
        match c {
            '0'..='9' => {
                current_num = current_num * 10 + (c as u16 - '0' as u16);
                if current_num > 255 {
                    return None;
                }
                has_digits = true;
            }
            '.' => {
                if !has_digits || part_idx >= 3 {
                    return None;
                }
                parts[part_idx] = current_num as u8;
                part_idx += 1;
                current_num = 0;
                has_digits = false;
            }
            _ => return None,
        }
    }
    
    // Save last octet
    if !has_digits || part_idx != 3 {
        return None;
    }
    parts[3] = current_num as u8;
    
    Some(parts)
}

/// Read a file from the PSP filesystem
fn read_file(path: &[u8]) -> Option<Vec<u8>> {
    unsafe {
        // Open file
        let fd: SceUid = sceIoOpen(
            path.as_ptr() as *const u8,
            IoOpenFlags::RD_ONLY,
            0o777,
        );
        if fd.0 < 0 {
            return None;
        }
        
        // Get file size
        let size = sceIoLseek(fd, 0, IoWhence::End);
        if size < 0 {
            sceIoClose(fd);
            return None;
        }
        
        // Seek back to start
        sceIoLseek(fd, 0, IoWhence::Set);
        
        // Read file content
        let mut buffer = Vec::with_capacity(size as usize);
        buffer.resize(size as usize, 0);
        
        let bytes_read = sceIoRead(fd, buffer.as_mut_ptr() as *mut c_void, size as u32);
        sceIoClose(fd);
        
        if bytes_read < 0 {
            return None;
        }
        
        buffer.truncate(bytes_read as usize);
        Some(buffer)
    }
}

/// Write a file to the PSP filesystem
fn write_file(path: &[u8], data: &[u8]) -> bool {
    unsafe {
        // Open file for writing (create/truncate)
        let fd: SceUid = sceIoOpen(
            path.as_ptr() as *const u8,
            IoOpenFlags::WR_ONLY | IoOpenFlags::CREAT | IoOpenFlags::TRUNC,
            0o777,
        );
        if fd.0 < 0 {
            return false;
        }
        
        // Write content
        let bytes_written = sceIoWrite(fd, data.as_ptr() as *const c_void, data.len());
        sceIoClose(fd);
        
        bytes_written >= 0 && bytes_written as usize == data.len()
    }
}

