//! PSP Stats App - Homebrew game statistics viewer
//!
//! Fetches game usage statistics from the desktop companion via WiFi
//! and displays them in a TUI interface.
//!
//! Based on rust-psp ratatui example using mousefood backend.

#![no_std]
#![no_main]

extern crate alloc;

mod config;
mod network;
mod stats;
mod ui;

use alloc::string::String;
use alloc::format;
use core::ffi::c_void;

use mousefood::{EmbeddedBackend, EmbeddedBackendConfig};
use psp::embedded_graphics::Framebuffer;
use embedded_graphics::pixelcolor::Rgb888;
use embedded_graphics::prelude::*;
use ratatui::Terminal;

use psp::sys::{
    sceIoOpen, sceIoWrite, sceIoClose,
    IoOpenFlags, SceUid,
};

use crate::config::{load_config, load_usage_json, save_usage_json};
use crate::network::NetworkManager;
use crate::stats::{parse_usage_json, sample_stats, StatsData};

psp::module!("PSP DRP", 1, 1);

/// Log file path
const LOG_PATH: &[u8] = b"ms0:/seplugins/pspdrp/stats_app.log\0";

/// Global logging enable flag (set from config)
static mut G_LOGGING_ENABLED: bool = false;

/// Enable logging (call after loading config)
fn set_logging_enabled(enabled: bool) {
    unsafe {
        G_LOGGING_ENABLED = enabled;
    }
}

/// Write a log message to file (only if logging enabled)
fn log(msg: &str) {
    unsafe {
        if !G_LOGGING_ENABLED {
            return;
        }
        let fd: SceUid = sceIoOpen(
            LOG_PATH.as_ptr() as *const u8,
            IoOpenFlags::WR_ONLY | IoOpenFlags::CREAT | IoOpenFlags::APPEND,
            0o777,
        );
        if fd.0 >= 0 {
            sceIoWrite(fd, msg.as_ptr() as *const c_void, msg.len());
            sceIoWrite(fd, b"\n".as_ptr() as *const c_void, 1);
            sceIoClose(fd);
        }
    }
}

/// Data source indicator
enum DataSource {
    Local,
    Network,
    Sample,
}

/// Application state
struct AppState {
    stats: StatsData,
    scroll_offset: usize,
    should_exit: bool,
    status_message: String,
    /// Selector mode: shows all games (including hidden) and allows toggling hide status
    selector_mode: bool,
    /// Currently selected game index (in filtered list)
    selected_index: usize,
}

impl AppState {
    fn new(stats: StatsData) -> Self {
        Self {
            stats,
            scroll_offset: 0,
            should_exit: false,
            status_message: String::new(),
            selector_mode: false,
            selected_index: 0,
        }
    }
    
    fn scroll_up(&mut self) {
        if self.scroll_offset > 0 {
            self.scroll_offset -= 1;
        }
    }
    
    fn scroll_down(&mut self) {
        let visible_count = self.get_visible_game_count();
        let max_scroll = visible_count.saturating_sub(5);
        if self.scroll_offset < max_scroll {
            self.scroll_offset += 1;
        }
    }
    
    /// Move selection up
    fn select_up(&mut self) {
        if self.selected_index > 0 {
            self.selected_index -= 1;
            // Scroll if needed to keep selection visible
            if self.selected_index < self.scroll_offset {
                self.scroll_offset = self.selected_index;
            }
        }
    }
    
    /// Move selection down
    fn select_down(&mut self) {
        let visible_count = self.get_visible_game_count();
        if self.selected_index + 1 < visible_count {
            self.selected_index += 1;
            // Scroll if needed to keep selection visible (assuming ~5 visible rows)
            if self.selected_index >= self.scroll_offset + 5 {
                self.scroll_offset = self.selected_index.saturating_sub(4);
            }
        }
    }
    
    /// Toggle selector mode
    fn toggle_selector_mode(&mut self) {
        self.selector_mode = !self.selector_mode;
        if self.selector_mode {
            // When entering selector mode, reset selection to current scroll position
            self.selected_index = self.scroll_offset;
        }
    }
    
    /// Get count of visible games (all in selector mode, non-hidden otherwise)
    fn get_visible_game_count(&self) -> usize {
        if self.selector_mode {
            self.stats.games.len()
        } else {
            self.stats.games.iter().filter(|g| !g.hidden).count()
        }
    }
    
    /// Get the game at the selected index (in the visible list)
    fn get_selected_game(&self) -> Option<&crate::stats::GameStats> {
        if self.selector_mode {
            self.stats.games.get(self.selected_index)
        } else {
            self.stats.games.iter()
                .filter(|g| !g.hidden)
                .nth(self.selected_index)
        }
    }
    
    /// Toggle hidden status for the selected game
    fn toggle_selected_hidden(&mut self) -> bool {
        if !self.selector_mode {
            return false;
        }
        
        if let Some(game) = self.stats.games.get_mut(self.selected_index) {
            let new_hidden = !game.hidden;
            let game_key = game.game_key.clone();
            game.hidden = new_hidden;
            
            // Update the JSON file
            crate::config::update_game_hidden(&game_key, new_hidden)
        } else {
            false
        }
    }
}


/// Load stats: try network if IP configured, fall back to cache or sample
/// After network fetch, saves to local cache and shuts down WiFi
fn load_stats() -> (StatsData, DataSource) {
    // 1. Load config first to check offline_mode and logging
    let config = load_config();
    set_logging_enabled(config.enable_logging);
    
    log("load_stats: starting");
    
    // If offline mode, just use local cache (no network)
    if config.offline_mode {
        log("load_stats: OFFLINE MODE - using local cache only");
        if let Some(json) = load_usage_json() {
            let stats = parse_usage_json(&json);
            if !stats.games.is_empty() {
                log(&format!("load_stats: local cache has {} games", stats.games.len()));
                return (stats, DataSource::Local);
            }
        }
        log("load_stats: no local cache, using sample data");
        return (sample_stats(), DataSource::Sample);
    }
    
    // 2. Load local cache as fallback (if exists)
    log("load_stats: checking local cache");
    let cached_stats = if let Some(json) = load_usage_json() {
        log("load_stats: found local cache, parsing");
        let stats = parse_usage_json(&json);
        if !stats.games.is_empty() {
            log(&format!("load_stats: cache has {} games", stats.games.len()));
            Some(stats)
        } else {
            log("load_stats: cache empty or invalid");
            None
        }
    } else {
        log("load_stats: no local cache found");
        None
    };
    
    // 3. Check config for IP - skip if no IP configured
    if !config.has_ip {
        log("load_stats: no IP configured");
        return match cached_stats {
            Some(stats) => {
                log("load_stats: using cached stats");
                (stats, DataSource::Local)
            }
            None => {
                log("load_stats: using sample data");
                (sample_stats(), DataSource::Sample)
            }
        };
    }
    
    // 3. IP configured - try to fetch fresh stats from network
    log(&format!("load_stats: IP={}.{}.{}.{} port={}", 
        config.desktop_ip[0], config.desktop_ip[1], 
        config.desktop_ip[2], config.desktop_ip[3], config.port));
    
    let mut network = NetworkManager::new();
    
    // Set IP and port from config
    network.set_companion_ip(config.desktop_ip);
    network.set_companion_port(config.port);
    
    // Try to initialize network
    log("load_stats: initializing network");
    if !network.init() {
        log("load_stats: network init failed");
        return match cached_stats {
            Some(stats) => {
                log("load_stats: using cached stats as fallback");
                (stats, DataSource::Local)
            }
            None => {
                log("load_stats: using sample data");
                (sample_stats(), DataSource::Sample)
            }
        };
    }
    log("load_stats: network init success");
    
    // Try to connect to WiFi using saved connection profile 1
    log("load_stats: connecting to WiFi");
    if !network.connect_wifi(1) {
        log("load_stats: WiFi connect failed, cleaning up");
        network.cleanup();
        log("load_stats: cleanup done");
        return match cached_stats {
            Some(stats) => {
                log("load_stats: using cached stats as fallback");
                (stats, DataSource::Local)
            }
            None => {
                log("load_stats: using sample data");
                (sample_stats(), DataSource::Sample)
            }
        };
    }
    log("load_stats: WiFi connected");
    
    // Create socket
    log("load_stats: creating socket");
    if !network.create_socket() {
        log("load_stats: socket creation failed, cleaning up");
        network.cleanup();
        log("load_stats: cleanup done");
        return match cached_stats {
            Some(stats) => {
                log("load_stats: using cached stats as fallback");
                (stats, DataSource::Local)
            }
            None => {
                log("load_stats: using sample data");
                (sample_stats(), DataSource::Sample)
            }
        };
    }
    log("load_stats: socket created");
    
    // Try to fetch stats (get raw JSON bytes)
    log("load_stats: fetching stats from companion");
    let result = match network.fetch_stats_raw() {
        Some((stats, json_bytes)) => {
            log(&format!("load_stats: received {} bytes, {} games", json_bytes.len(), stats.games.len()));
            // Save to local cache
            log("load_stats: saving to cache");
            let saved = save_usage_json(&json_bytes);
            log(&format!("load_stats: cache save result: {}", saved));
            (stats, DataSource::Network)
        }
        None => {
            log("load_stats: fetch failed");
            match cached_stats {
                Some(stats) => {
                    log("load_stats: using cached stats as fallback");
                    (stats, DataSource::Local)
                }
                None => {
                    log("load_stats: using sample data");
                    (sample_stats(), DataSource::Sample)
                }
            }
        }
    };
    
    // Always cleanup network after fetch (turn off WiFi, unload modules)
    log("load_stats: cleaning up network");
    network.cleanup();
    log("load_stats: network cleanup done");
    
    result
}

/// Handle D-pad input for scrolling and selector mode
fn handle_input(state: &mut AppState) {
    unsafe {
        // Read controller state
        let mut pad_data: psp::sys::SceCtrlData = core::mem::zeroed();
        psp::sys::sceCtrlReadBufferPositive(&mut pad_data, 1);
        
        // Square button - toggle selector mode
        if pad_data.buttons.contains(psp::sys::CtrlButtons::SQUARE) {
            state.toggle_selector_mode();
        }
        
        // In selector mode, use different navigation
        if state.selector_mode {
            // X button - toggle hidden status on selected entry
            if pad_data.buttons.contains(psp::sys::CtrlButtons::CROSS) {
                if state.toggle_selected_hidden() {
                    // Update status message
                    if let Some(game) = state.get_selected_game() {
                        state.status_message = if game.hidden {
                            format!("Hidden: {}", game.title)
                        } else {
                            format!("Shown: {}", game.title)
                        };
                    }
                }
            }
            
            // D-pad navigates selection in selector mode
            if pad_data.buttons.contains(psp::sys::CtrlButtons::UP) {
                state.select_up();
            }
            if pad_data.buttons.contains(psp::sys::CtrlButtons::DOWN) {
                state.select_down();
            }
        } else {
            // Normal mode: D-pad scrolls
            if pad_data.buttons.contains(psp::sys::CtrlButtons::UP) {
                state.scroll_up();
            }
            if pad_data.buttons.contains(psp::sys::CtrlButtons::DOWN) {
                state.scroll_down();
            }
        }
        
        // Simple debounce - wait a bit between inputs
        psp::sys::sceKernelDelayThread(100_000); // 100ms
    }
}


fn psp_main() {
    psp::enable_home_button();
    
    log("=== PSP Stats App Starting ===");
    
    // Set up controller sampling
    unsafe {
        psp::sys::sceCtrlSetSamplingCycle(0);
        psp::sys::sceCtrlSetSamplingMode(psp::sys::CtrlMode::Analog);
    }
    log("psp_main: controller initialized");
    
    // Load stats - tries local cache first, then network
    log("psp_main: calling load_stats");
    let (stats, source) = load_stats();
    log(&format!("psp_main: load_stats returned, {} games", stats.games.len()));
    
    let mut app_state = AppState::new(stats);
    
    app_state.status_message = match source {
        DataSource::Local => String::from("Loaded from cache"),
        DataSource::Network => String::from("Fetched from companion"),
        DataSource::Sample => String::from("Using sample data"),
    };
    log(&format!("psp_main: status={}", app_state.status_message));
    
    // Initialize display
    log("psp_main: initializing display");
    let mut disp = Framebuffer::new();
    disp.clear(Rgb888::BLACK).unwrap();
    log("psp_main: display cleared");
    
    // Create ratatui terminal with mousefood backend
    log("psp_main: creating terminal");
    let backend = EmbeddedBackend::new(&mut disp, EmbeddedBackendConfig::default());
    let mut terminal = Terminal::new(backend).unwrap();
    log("psp_main: terminal created");
    
    // Main loop
    log("psp_main: entering main loop");
    loop {
        // Render UI
        terminal.draw(|frame| {
            ui::render_stats(frame, &app_state.stats, app_state.scroll_offset, 
                app_state.selector_mode, app_state.selected_index);
        }).unwrap();
        
        // Handle input
        handle_input(&mut app_state);
        
        if app_state.should_exit {
            break;
        }
        
        // Small delay to prevent busy-waiting
        unsafe {
            psp::sys::sceKernelDelayThread(16_666); // ~60fps
        }
    }
    
    log("psp_main: exiting");
}
