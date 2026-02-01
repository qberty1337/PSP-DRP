//! Usage Tracker Module
//!
//! Tracks and logs game/app usage sessions to a JSON file.
//! Sessions accumulate playtime and are updated on each game update.

use std::collections::HashMap;
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use chrono::Local;
use serde::{Deserialize, Serialize};
use tracing::{debug, info, warn};

use crate::protocol::{GameInfo, PspState};

/// A tracked game/app with cumulative playtime
#[derive(Debug, Clone, Serialize, Deserialize)]
struct TrackedGame {
    /// Game/app ID (e.g., "UCUS98632" or "gpSP")
    game_id: String,
    /// Display title
    title: String,
    /// Total accumulated playtime in seconds
    total_seconds: u64,
    /// First played timestamp
    #[serde(default)]
    first_played: String,
    /// Last played timestamp
    last_played: String,
    /// Number of sessions
    session_count: u32,
    /// Dates when this game was played (YYYY-MM-DD format)
    #[serde(default)]
    play_dates: std::collections::HashSet<String>,
}

/// Per-PSP usage data
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct PspUsageData {
    /// PSP name
    psp_name: String,
    /// Games/apps tracked for this PSP
    games: HashMap<String, TrackedGame>,
}

/// In-memory session tracking
#[derive(Debug)]
struct ActiveSession {
    game_id: String,
    title: String,
    state: PspState,
    start_time: Instant,
    /// How many seconds have already been saved to disk for this session
    saved_seconds: u64,
}

/// Per-PSP tracker (in-memory state)
#[derive(Debug)]
struct PspTracker {
    name: String,
    current_session: Option<ActiveSession>,
}

/// Usage data file structure
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct UsageData {
    /// Per-PSP usage data, keyed by PSP name
    psps: HashMap<String, PspUsageData>,
}

/// Usage tracker that records game sessions
pub struct UsageTracker {
    /// Per-PSP tracking state (in-memory, by socket address)
    trackers: HashMap<SocketAddr, PspTracker>,
    /// Path to the usage data file (JSON)
    data_path: PathBuf,
    /// Whether tracking is enabled
    enabled: bool,
    /// Minimum session duration to track (avoids spam on quick switches)
    min_session_seconds: u64,
}

impl UsageTracker {
    /// Create a new usage tracker
    pub fn new(log_path: PathBuf, enabled: bool) -> Self {
        // Change extension to .json for the new format
        let data_path = log_path.with_extension("json");
        
        Self {
            trackers: HashMap::new(),
            data_path,
            enabled,
            min_session_seconds: 1,
        }
    }

    /// Get the default log path (beside the executable)
    pub fn default_log_path() -> PathBuf {
        std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|dir| dir.join("usage_log.txt")))
            .unwrap_or_else(|| PathBuf::from("usage_log.txt"))
    }

    /// Load usage data from file
    fn load_data(&self) -> UsageData {
        if let Ok(mut file) = fs::File::open(&self.data_path) {
            let mut contents = String::new();
            if file.read_to_string(&mut contents).is_ok() {
                if let Ok(data) = serde_json::from_str(&contents) {
                    return data;
                }
            }
        }
        UsageData::default()
    }

    /// Save usage data to file
    fn save_data(&self, data: &UsageData) {
        match OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&self.data_path)
        {
            Ok(mut file) => {
                if let Ok(json) = serde_json::to_string_pretty(data) {
                    if let Err(e) = file.write_all(json.as_bytes()) {
                        warn!("Usage tracker: failed to write data: {}", e);
                    }
                }
            }
            Err(e) => {
                warn!("Usage tracker: failed to open data file: {}", e);
            }
        }
    }

    /// Register a PSP connection or update its name if already registered
    pub fn register_psp(&mut self, addr: SocketAddr, name: String) {
        if !self.enabled {
            return;
        }

        if let Some(tracker) = self.trackers.get_mut(&addr) {
            if tracker.name != name {
                debug!(
                    "Usage tracker: updating PSP name from '{}' to '{}'",
                    tracker.name, name
                );
                tracker.name = name;
            }
        } else {
            self.trackers.insert(
                addr,
                PspTracker {
                    name: name.clone(),
                    current_session: None,
                },
            );
            debug!("Usage tracker: registered PSP {} as '{}'", addr, name);
        }
    }

    /// Unregister a PSP and save any active session
    pub fn unregister_psp(&mut self, addr: SocketAddr) {
        if !self.enabled {
            return;
        }

        if let Some(mut tracker) = self.trackers.remove(&addr) {
            if let Some(session) = tracker.current_session.take() {
                self.save_session(&tracker.name, &session);
            }
            debug!("Usage tracker: unregistered PSP {}", addr);
        }
    }

    /// Update game info for a PSP - saves session duration on each update
    pub fn update_game(&mut self, addr: SocketAddr, info: &GameInfo) {
        if !self.enabled {
            return;
        }

        // Determine the PSP name - use from GameInfo if available
        let psp_name_from_packet = if !info.psp_name.is_empty() {
            info.psp_name.clone()
        } else {
            format!("PSP-{}", addr.port())
        };

        // Ensure the PSP is registered, updating name if we have a better one
        if let Some(tracker) = self.trackers.get_mut(&addr) {
            if !info.psp_name.is_empty() && tracker.name != info.psp_name {
                debug!(
                    "Usage tracker: updating PSP name from '{}' to '{}'",
                    tracker.name, info.psp_name
                );
                tracker.name = info.psp_name.clone();
            }
        } else {
            self.trackers.insert(
                addr,
                PspTracker {
                    name: psp_name_from_packet.clone(),
                    current_session: None,
                },
            );
        }

        // Check if game changed and extract data for logging
        let (game_changed, session_to_save, psp_name_for_log) = {
            let tracker = self.trackers.get(&addr).unwrap();
            let changed = match &tracker.current_session {
                Some(session) => session.game_id != info.game_id || session.state != info.state,
                None => true,
            };
            
            // If same game, get session data for saving
            let session_data = if !changed {
                tracker.current_session.as_ref().map(|s| ActiveSession {
                    game_id: s.game_id.clone(),
                    title: s.title.clone(),
                    state: s.state,
                    start_time: s.start_time,
                    saved_seconds: s.saved_seconds,
                })
            } else {
                None
            };
            
            (changed, session_data, tracker.name.clone())
        };

        // Save current session if game hasn't changed
        if let Some(session) = session_to_save {
            let new_saved_seconds = self.save_session(&psp_name_for_log, &session);
            // Update the tracker's saved_seconds
            if let Some(tracker) = self.trackers.get_mut(&addr) {
                if let Some(ref mut current) = tracker.current_session {
                    current.saved_seconds = new_saved_seconds;
                }
            }
        }

        // Handle game change
        if game_changed {
            // Extract and save the previous session
            let prev_session_data = {
                let tracker = self.trackers.get_mut(&addr).unwrap();
                tracker.current_session.take().map(|s| {
                    (tracker.name.clone(), s)
                })
            };
            
            if let Some((name, prev_session)) = prev_session_data {
                self.finish_session(&name, &prev_session);
                info!(
                    "Usage tracker: ended session for '{}' on {}",
                    prev_session.title, name
                );
            }

            // Start new session
            let tracker = self.trackers.get_mut(&addr).unwrap();
            tracker.current_session = Some(ActiveSession {
                game_id: info.game_id.clone(),
                title: info.title.clone(),
                state: info.state,
                start_time: Instant::now(),
                saved_seconds: 0,
            });

            debug!(
                "Usage tracker: started session for '{}' on {}",
                info.title, tracker.name
            );
        }
    }

    /// Save a session's playtime delta to the usage data file
    /// Returns the new total saved_seconds for this session
    fn save_session(&self, psp_name: &str, session: &ActiveSession) -> u64 {
        let total_elapsed = session.start_time.elapsed().as_secs();
        
        // Calculate delta (time since last save)
        let delta = total_elapsed.saturating_sub(session.saved_seconds);
        
        // Skip if no new time to save
        if delta < self.min_session_seconds {
            return session.saved_seconds;
        }

        let mut data = self.load_data();

        // Get or create PSP entry
        let psp_data = data.psps.entry(psp_name.to_string()).or_insert_with(|| {
            PspUsageData {
                psp_name: psp_name.to_string(),
                games: HashMap::new(),
            }
        });

        // Get or create game entry
        let game_key = format!("{}:{}", session.game_id, session.state as u8);
        let now = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        let today = Local::now().format("%Y-%m-%d").to_string();
        let game_data = psp_data.games.entry(game_key.clone()).or_insert_with(|| {
            TrackedGame {
                game_id: session.game_id.clone(),
                title: session.title.clone(),
                total_seconds: 0,
                first_played: now.clone(),
                last_played: String::new(),
                session_count: 0,
                play_dates: std::collections::HashSet::new(),
            }
        });

        // Add delta to total (not replace)
        game_data.total_seconds += delta;
        game_data.last_played = now;
        game_data.play_dates.insert(today);
        let total_for_log = game_data.total_seconds;

        self.save_data(&data);

        debug!(
            "Usage tracker: saved +{} (total {}) for '{}' on {}",
            format_duration(Duration::from_secs(delta)),
            format_duration(Duration::from_secs(total_for_log)),
            session.title,
            psp_name
        );
        
        total_elapsed
    }

    /// Finish a session and increment the session count
    fn finish_session(&self, psp_name: &str, session: &ActiveSession) {
        let total_elapsed = session.start_time.elapsed().as_secs();
        
        // Calculate remaining unsaved time
        let delta = total_elapsed.saturating_sub(session.saved_seconds);

        // Skip if session is too short overall
        if total_elapsed < self.min_session_seconds {
            return;
        }

        let mut data = self.load_data();

        let psp_data = data.psps.entry(psp_name.to_string()).or_insert_with(|| {
            PspUsageData {
                psp_name: psp_name.to_string(),
                games: HashMap::new(),
            }
        });

        let game_key = format!("{}:{}", session.game_id, session.state as u8);
        let now = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        let today = Local::now().format("%Y-%m-%d").to_string();
        let game_data = psp_data.games.entry(game_key).or_insert_with(|| {
            TrackedGame {
                game_id: session.game_id.clone(),
                title: session.title.clone(),
                total_seconds: 0,
                first_played: now.clone(),
                last_played: String::new(),
                session_count: 0,
                play_dates: std::collections::HashSet::new(),
            }
        });

        // Add only the remaining delta (unsaved time) to total
        game_data.total_seconds += delta;
        game_data.last_played = now;
        game_data.session_count += 1;
        game_data.play_dates.insert(today);
        let total_for_log = game_data.total_seconds;

        self.save_data(&data);

        info!(
            "Usage tracker: finished session for '{}' (+{}, session: {}) - Total: {}",
            session.title,
            format_duration(Duration::from_secs(delta)),
            format_duration(Duration::from_secs(total_elapsed)),
            format_duration(Duration::from_secs(total_for_log))
        );
    }

    /// Flush all active sessions (call on shutdown)
    pub fn flush_all(&mut self) {
        if !self.enabled {
            return;
        }

        let addrs: Vec<SocketAddr> = self.trackers.keys().cloned().collect();
        for addr in addrs {
            if let Some(mut tracker) = self.trackers.remove(&addr) {
                if let Some(session) = tracker.current_session.take() {
                    self.finish_session(&tracker.name, &session);
                }
            }
        }
        info!("Usage tracker: flushed all sessions");
    }

    /// Get the top N most played games across all PSPs
    /// Returns a vector of (title, total_seconds), sorted by playtime descending
    pub fn get_top_played(&self, count: usize) -> Vec<(String, u64)> {
        let data = self.load_data();
        
        // Collect all games with their playtimes
        let mut all_games: Vec<(String, u64)> = Vec::new();
        
        for psp_data in data.psps.values() {
            for game in psp_data.games.values() {
                // Skip entries with no title
                if game.title.is_empty() {
                    continue;
                }
                
                // Check if we already have this title (aggregate across PSPs)
                if let Some(existing) = all_games.iter_mut().find(|(t, _)| t == &game.title) {
                    existing.1 += game.total_seconds;
                } else {
                    all_games.push((game.title.clone(), game.total_seconds));
                }
            }
        }
        
        // Sort by playtime descending
        all_games.sort_by(|a, b| b.1.cmp(&a.1));
        
        // Take top N
        all_games.truncate(count);
        
        all_games
    }

    /// Get all game stats across all PSPs
    /// Returns a vector of detailed game stats, sorted by playtime descending
    pub fn get_all_game_stats(&self) -> Vec<(String, String, u64, u32, String)> {
        let data = self.load_data();
        
        // Use a hashmap to aggregate by title
        use std::collections::HashMap;
        let mut game_map: HashMap<String, (String, u64, u32, String)> = HashMap::new();
        
        for psp_data in data.psps.values() {
            for game in psp_data.games.values() {
                // Skip entries with no title
                if game.title.is_empty() {
                    continue;
                }
                
                // Aggregate by title
                if let Some(existing) = game_map.get_mut(&game.title) {
                    existing.1 += game.total_seconds;
                    existing.2 += game.session_count;
                    // Keep the most recent last_played
                    if game.last_played > existing.3 {
                        existing.3 = game.last_played.clone();
                    }
                } else {
                    game_map.insert(game.title.clone(), (
                        game.game_id.clone(),
                        game.total_seconds,
                        game.session_count,
                        game.last_played.clone(),
                    ));
                }
            }
        }
        
        // Convert to vector and sort by playtime descending
        let mut all_games: Vec<(String, String, u64, u32, String)> = game_map
            .into_iter()
            .map(|(title, (game_id, seconds, sessions, last_played))| {
                (title, game_id, seconds, sessions, last_played)
            })
            .collect();
        
        all_games.sort_by(|a, b| b.2.cmp(&a.2));
        
        all_games
    }

    /// Get all play dates with the games played on each date
    /// Returns a HashMap mapping date (YYYY-MM-DD) to list of game titles played that day
    pub fn get_all_play_dates(&self) -> std::collections::HashMap<String, Vec<String>> {
        let data = self.load_data();
        let mut date_games: std::collections::HashMap<String, Vec<String>> = std::collections::HashMap::new();
        
        for psp_data in data.psps.values() {
            for game in psp_data.games.values() {
                // Skip entries with no title
                if game.title.is_empty() {
                    continue;
                }
                
                for date in &game.play_dates {
                    date_games.entry(date.clone())
                        .or_insert_with(Vec::new)
                        .push(game.title.clone());
                }
                
                // Also extract dates from first_played and last_played for backwards compatibility
                if game.first_played.len() >= 10 {
                    let date = game.first_played[..10].to_string();
                    date_games.entry(date)
                        .or_insert_with(Vec::new)
                        .push(game.title.clone());
                }
                if game.last_played.len() >= 10 {
                    let date = game.last_played[..10].to_string();
                    date_games.entry(date)
                        .or_insert_with(Vec::new)
                        .push(game.title.clone());
                }
            }
        }
        
        date_games
    }
}

/// Format a duration as human-readable string
fn format_duration(duration: Duration) -> String {
    let total_secs = duration.as_secs();
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;

    if hours > 0 {
        format!("{}h {}m {}s", hours, mins, secs)
    } else if mins > 0 {
        format!("{}m {}s", mins, secs)
    } else {
        format!("{}s", secs)
    }
}
