//! Stats data structures and JSON parsing for usage.json
//!
//! The usage.json format comes from the desktop companion:
//! ```json
//! {
//!   "psps": {
//!     "PSP Name": {
//!       "psp_name": "PSP Name",
//!       "games": {
//!         "GAMEID:1": {
//!           "game_id": "GAMEID",
//!           "title": "Game Title",
//!           "total_seconds": 3600,
//!           "session_count": 5,
//!           "last_played": "2025-01-29 22:15:31"
//!         }
//!       }
//!     }
//!   }
//! }
//! ```

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

/// Individual game statistics
#[derive(Clone)]
pub struct GameStats {
    pub title: String,
    pub game_id: String,
    pub game_key: String,  // Full key like "UCUS98712:1" for JSON updates
    pub total_seconds: u64,
    pub session_count: u32,
    pub last_played: String,
    pub hidden: bool,
}

/// All loaded stats data
pub struct StatsData {
    pub games: Vec<GameStats>,
    pub total_playtime: u64,
    pub total_sessions: u32,
}

impl Default for StatsData {
    fn default() -> Self {
        Self {
            games: Vec::new(),
            total_playtime: 0,
            total_sessions: 0,
        }
    }
}

/// Simple string extraction helper for JSON parsing
fn extract_string_value(json: &str, key: &str) -> Option<String> {
    let search = alloc::format!("\"{}\":", key);
    let start = json.find(&search)?;
    let after_key = &json[start + search.len()..];
    
    // Skip whitespace
    let trimmed = after_key.trim_start();
    
    if trimmed.starts_with('"') {
        // String value
        let content = &trimmed[1..];
        let end = content.find('"')?;
        Some(String::from(&content[..end]))
    } else {
        None
    }
}

/// Simple number extraction helper for JSON parsing
fn extract_number_value(json: &str, key: &str) -> Option<u64> {
    let search = alloc::format!("\"{}\":", key);
    let start = json.find(&search)?;
    let after_key = &json[start + search.len()..];
    
    // Skip whitespace
    let trimmed = after_key.trim_start();
    
    // Parse number
    let mut num_str = String::new();
    for c in trimmed.chars() {
        if c.is_ascii_digit() {
            num_str.push(c);
        } else {
            break;
        }
    }
    
    num_str.parse().ok()
}

/// Simple boolean extraction helper for JSON parsing
fn extract_bool_value(json: &str, key: &str) -> Option<bool> {
    let search = alloc::format!("\"{}\":", key);
    let start = json.find(&search)?;
    let after_key = &json[start + search.len()..];
    
    // Skip whitespace
    let trimmed = after_key.trim_start();
    
    if trimmed.starts_with("true") {
        Some(true)
    } else if trimmed.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

/// Parse a single game entry from JSON
/// Supports both old format (total_seconds, session_count) and new network format (seconds, sessions)
fn parse_game_entry(json: &str, game_key: String) -> Option<GameStats> {
    let title = extract_string_value(json, "title")?;
    let game_id = extract_string_value(json, "game_id").unwrap_or_default();
    
    // Try new format first (seconds, sessions), then old format (total_seconds, session_count)
    let total_seconds = extract_number_value(json, "seconds")
        .or_else(|| extract_number_value(json, "total_seconds"))
        .unwrap_or(0);
    let session_count = extract_number_value(json, "sessions")
        .or_else(|| extract_number_value(json, "session_count"))
        .unwrap_or(0) as u32;
    let last_played = extract_string_value(json, "last_played").unwrap_or_default();
    let hidden = extract_bool_value(json, "hidden").unwrap_or(false);
    
    Some(GameStats {
        title,
        game_id,
        game_key,
        total_seconds,
        session_count,
        last_played,
        hidden,
    })
}

/// Parse usage.json content into StatsData
pub fn parse_usage_json(json: &str) -> StatsData {
    let mut stats = StatsData::default();
    
    // Find all game entries by looking for "title": patterns
    // This is a simple approach that works for our specific JSON format
    let mut search_start = 0;
    
    while let Some(title_pos) = json[search_start..].find("\"title\":") {
        let abs_pos = search_start + title_pos;
        
        // Find the enclosing braces for this game object
        // Look backwards for the opening brace
        let mut brace_count = 0;
        let mut obj_start = abs_pos;
        for (i, c) in json[..abs_pos].chars().rev().enumerate() {
            match c {
                '}' => brace_count += 1,
                '{' => {
                    if brace_count == 0 {
                        obj_start = abs_pos - i - 1;
                        break;
                    }
                    brace_count -= 1;
                }
                _ => {}
            }
        }
        
        // Extract game_key by looking for the quoted key before the opening brace
        // Format is: "GAMEID:1": { ... }
        let game_key = extract_game_key(&json[..obj_start]);
        
        // Find the closing brace
        let mut brace_count = 1;
        let mut obj_end = abs_pos;
        for (i, c) in json[obj_start + 1..].chars().enumerate() {
            match c {
                '{' => brace_count += 1,
                '}' => {
                    brace_count -= 1;
                    if brace_count == 0 {
                        obj_end = obj_start + 1 + i + 1;
                        break;
                    }
                }
                _ => {}
            }
        }
        
        // Parse the game object
        let game_json = &json[obj_start..obj_end];
        if let Some(game) = parse_game_entry(game_json, game_key) {
            // Skip empty titles
            if !game.title.is_empty() {
                stats.total_playtime += game.total_seconds;
                stats.total_sessions += game.session_count;
                stats.games.push(game);
            }
        }
        
        search_start = obj_end;
    }
    
    // Sort games by playtime (descending)
    stats.games.sort_by(|a, b| b.total_seconds.cmp(&a.total_seconds));
    
    stats
}

/// Extract the game key (e.g., "UCUS98712:1") from before the opening brace
fn extract_game_key(before_brace: &str) -> String {
    // Look for pattern: "KEY": at the end of the string
    // Work backwards to find the last colon, then find the quoted key before it
    let trimmed = before_brace.trim_end();
    
    // Should end with a colon (after the key, before the {)
    if !trimmed.ends_with(':') {
        return String::new();
    }
    
    // Find the closing quote of the key
    let before_colon = &trimmed[..trimmed.len()-1].trim_end();
    if !before_colon.ends_with('"') {
        return String::new();
    }
    
    // Find the opening quote of the key
    let key_end = before_colon.len() - 1;
    if let Some(key_start) = before_colon[..key_end].rfind('"') {
        String::from(&before_colon[key_start + 1..key_end])
    } else {
        String::new()
    }
}

/// Format duration as human-readable string (e.g., "12h 34m 56s")
pub fn format_duration(total_secs: u64) -> String {
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;
    
    if hours > 0 {
        alloc::format!("{}h {}m {}s", hours, mins, secs)
    } else if mins > 0 {
        alloc::format!("{}m {}s", mins, secs)
    } else {
        alloc::format!("{}s", secs)
    }
}

/// Format duration as short string (e.g., "12h" or "45m")
pub fn format_duration_short(total_secs: u64) -> String {
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    
    if hours > 0 {
        alloc::format!("{}h", hours)
    } else if mins > 0 {
        alloc::format!("{}m", mins)
    } else {
        String::from("<1m")
    }
}

/// Create sample stats data for testing
pub fn sample_stats() -> StatsData {
    let games = alloc::vec![
        GameStats {
            title: String::from("WipEout Pulse"),
            game_id: String::from("UCUS98712"),
            game_key: String::from("UCUS98712:1"),
            total_seconds: 3600 * 18 + 60 * 28,
            session_count: 16,
            last_played: String::from("01-31 23:04"),
            hidden: false,
        },
        GameStats {
            title: String::from("Corpse Party"),
            game_id: String::from("ULUS10566"),
            game_key: String::from("ULUS10566:1"),
            total_seconds: 3600 * 6 + 60 * 1,
            session_count: 4,
            last_played: String::from("01-31 23:18"),
            hidden: false,
        },
        GameStats {
            title: String::from("Secret Agent Clank"),
            game_id: String::from("UCUS98694"),
            game_key: String::from("UCUS98694:1"),
            total_seconds: 60 * 52,
            session_count: 2,
            last_played: String::from("02-01 01:16"),
            hidden: false,
        },
        GameStats {
            title: String::from("Manhunt 2"),
            game_id: String::from("ULUS10280"),
            game_key: String::from("ULUS10280:1"),
            total_seconds: 60 * 52,
            session_count: 2,
            last_played: String::from("02-01 01:16"),
            hidden: false,
        },
        GameStats {
            title: String::from("NFS Carbon OTC"),
            game_id: String::from("ULUS10168"),
            game_key: String::from("ULUS10168:1"),
            total_seconds: 60 * 16,
            session_count: 1,
            last_played: String::from("02-01 01:46"),
            hidden: false,
        },
        GameStats {
            title: String::from("MGS Portable Ops"),
            game_id: String::from("ULUS10202"),
            game_key: String::from("ULUS10202:1"),
            total_seconds: 60 * 56,
            session_count: 1,
            last_played: String::from("01-21 21:18"),
            hidden: false,
        },
    ];
    
    let total_playtime: u64 = games.iter().map(|g| g.total_seconds).sum();
    let total_sessions: u32 = games.iter().map(|g| g.session_count).sum();
    
    StatsData {
        games,
        total_playtime,
        total_sessions,
    }
}
