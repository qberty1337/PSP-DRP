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
    /// Last played timestamp
    last_played: String,
    /// Number of sessions
    session_count: u32,
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
                })
            } else {
                None
            };
            
            (changed, session_data, tracker.name.clone())
        };

        // Save current session if game hasn't changed
        if let Some(session) = session_to_save {
            self.save_session(&psp_name_for_log, &session);
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
            });

            debug!(
                "Usage tracker: started session for '{}' on {}",
                info.title, tracker.name
            );
        }
    }

    /// Save a session's playtime to the usage data file
    fn save_session(&self, psp_name: &str, session: &ActiveSession) {
        let duration = session.start_time.elapsed();

        // Skip very short sessions
        if duration.as_secs() < self.min_session_seconds {
            return;
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
        let game_data = psp_data.games.entry(game_key.clone()).or_insert_with(|| {
            TrackedGame {
                game_id: session.game_id.clone(),
                title: session.title.clone(),
                total_seconds: 0,
                last_played: String::new(),
                session_count: 0,
            }
        });

        // Update with current session duration
        game_data.total_seconds = duration.as_secs();
        game_data.last_played = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        let total_for_log = game_data.total_seconds;
        // Only increment session count on new sessions (handled in update_game)

        self.save_data(&data);

        debug!(
            "Usage tracker: saved {} total for '{}' on {}",
            format_duration(Duration::from_secs(total_for_log)),
            session.title,
            psp_name
        );
    }

    /// Finish a session and increment the session count
    fn finish_session(&self, psp_name: &str, session: &ActiveSession) {
        let duration = session.start_time.elapsed();

        if duration.as_secs() < self.min_session_seconds {
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
        let game_data = psp_data.games.entry(game_key).or_insert_with(|| {
            TrackedGame {
                game_id: session.game_id.clone(),
                title: session.title.clone(),
                total_seconds: 0,
                last_played: String::new(),
                session_count: 0,
            }
        });

        // Add session duration to total (not replace)
        game_data.total_seconds += duration.as_secs();
        game_data.last_played = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        game_data.session_count += 1;
        let total_for_log = game_data.total_seconds;

        self.save_data(&data);

        info!(
            "Usage tracker: finished session for '{}' ({}) - Total: {}",
            session.title,
            format_duration(duration),
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
