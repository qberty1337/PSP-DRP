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
    /// USB mode - offline mode + desktop companion auto-syncs
    pub usb_mode: bool,
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
            usb_mode: false,
            enable_logging: false,
        }
    }
}

/// INI file path on PSP memory stick
const INI_PATH: &[u8] = b"ms0:/seplugins/pspdrp/psp_drp.ini\0";

/// Usage JSON file path on PSP memory stick
const USAGE_PATH: &[u8] = b"ms0:/seplugins/pspdrp/usage_log.json\0";

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
                "usb_mode" => {
                    config.usb_mode = value == "1" || value.eq_ignore_ascii_case("true");
                    // USB mode implies offline mode
                    if config.usb_mode {
                        config.offline_mode = true;
                    }
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

/// Update the hidden flag for a specific game in usage.json
/// Returns true if successful
pub fn update_game_hidden(game_key: &str, hidden: bool) -> bool {
    // Load existing JSON
    let json = match load_usage_json() {
        Some(j) => j,
        None => return false,
    };
    
    // Find the game entry by game_key (format: "GAMEID:1": { ... })
    let search_pattern = alloc::format!("\"{}\":", game_key);
    let key_pos = match json.find(&search_pattern) {
        Some(pos) => pos,
        None => return false,
    };
    
    // Find the opening brace after the key
    let after_key = &json[key_pos + search_pattern.len()..];
    let brace_offset = match after_key.find('{') {
        Some(pos) => pos,
        None => return false,
    };
    let obj_start = key_pos + search_pattern.len() + brace_offset;
    
    // Find the closing brace for this game object
    let mut brace_count = 1;
    let mut obj_end = obj_start + 1;
    for (i, c) in json[obj_start + 1..].chars().enumerate() {
        match c {
            '{' => brace_count += 1,
            '}' => {
                brace_count -= 1;
                if brace_count == 0 {
                    obj_end = obj_start + 1 + i;
                    break;
                }
            }
            _ => {}
        }
    }
    
    let game_obj = &json[obj_start..obj_end + 1];
    
    // Build the new game object with updated hidden field
    let hidden_str = if hidden { "true" } else { "false" };
    
    let new_game_obj = if let Some(hidden_pos) = game_obj.find("\"hidden\":") {
        // Replace existing hidden value
        let relative_pos = hidden_pos;
        let after_hidden_key = &game_obj[relative_pos + 9..]; // 9 = len of "hidden":
        
        // Find end of the value (until comma, newline, or closing brace)
        let mut value_end = 0;
        for (i, c) in after_hidden_key.chars().enumerate() {
            match c {
                ',' | '\n' | '\r' | '}' => {
                    value_end = i;
                    break;
                }
                _ => value_end = i + 1,
            }
        }
        
        // Build new string: before hidden + "hidden": value + after value
        let before = &game_obj[..relative_pos + 9]; // includes "hidden":
        let after = &after_hidden_key[value_end..];
        alloc::format!("{} {}{}", before, hidden_str, after)
    } else {
        // Insert hidden field after the opening brace
        let insert_pos = 1; // After '{'
        let before = &game_obj[..insert_pos];
        let after = &game_obj[insert_pos..];
        
        // Check if there's content after the brace
        let trimmed_after = after.trim_start();
        if trimmed_after.starts_with('"') || trimmed_after.starts_with('\n') {
            // There's existing content, add hidden before it
            alloc::format!("{}\n      \"hidden\": {},\n     {}", before, hidden_str, after.trim_start())
        } else {
            alloc::format!("{} \"hidden\": {}, {}", before, hidden_str, after)
        }
    };
    
    // Build the new JSON
    let new_json = alloc::format!("{}{}{}", &json[..obj_start], new_game_obj, &json[obj_end + 1..]);
    
    // Save it
    save_usage_json(new_json.as_bytes())
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

