//! PSP Discord Rich Presence - Desktop Companion
//!
//! A fancy TUI application that receives data from a PSP running the
//! PSP DRP plugin and updates Discord Rich Presence accordingly.

mod ascii_art;
mod config;
mod discord;
mod protocol;
mod server;
mod thumbnail_matcher;
mod tui;
mod usage_tracker;
mod usb_transport;

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::Result;
use tokio::sync::{mpsc, RwLock};
use tracing::error;

use ascii_art::{IconManager, IconMode};
use config::Config;
use discord::DiscordManager;
use server::{Server, ServerCommand, ServerEvent};
use thumbnail_matcher::ThumbnailMatcher;
use tui::{GameStats, Tui, TuiEvent, TuiState, ViewMode};
use usage_tracker::UsageTracker;
use usb_transport::{UsbCommand, UsbEvent, spawn_usb_task};

/// Application refresh rate (10 Hz)
const REFRESH_INTERVAL_MS: u64 = 100;

/// Input poll timeout
const INPUT_POLL_MS: u64 = 10;

/// Discord reconnection interval
const DISCORD_RECONNECT_INTERVAL_SECS: u64 = 5;

#[tokio::main]
async fn main() -> Result<()> {
    // Initialize the TUI first (before any logging that might corrupt the display)
    let mut tui = Tui::new()?;
    let tui_state = Arc::new(RwLock::new(TuiState::new()));

    // Draw initial screen immediately
    {
        let state = tui_state.read().await;
        tui.draw(&state)?;
    }

    // Update status
    {
        let mut state = tui_state.write().await;
        state.status_message = "Loading configuration...".to_string();
        state.log_info("PSP DRP Desktop Companion starting...");
    }
    tui.draw(&tui_state.read().await.clone())?;

    // Load configuration
    let config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            let mut state = tui_state.write().await;
            state.log_error(&format!("Failed to load config: {}", e));
            state.status_message = "Config error! Press Q to quit.".to_string();
            drop(state);
            
            // Show error and wait for quit
            loop {
                let state = tui_state.read().await;
                tui.draw(&state)?;
                if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100), &state)? {
                    return Ok(());
                }
            }
        }
    };

    // Validate config
    if let Err(e) = config.validate() {
        let mut state = tui_state.write().await;
        state.log_error(&format!("Invalid config: {}", e));
        state.status_message = "Config invalid! Press Q to quit.".to_string();
        drop(state);
        
        loop {
            let state = tui_state.read().await;
            tui.draw(&state)?;
            if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100), &state)? {
                return Ok(());
            }
        }
    }

    {
        let mut state = tui_state.write().await;
        state.log_success("Configuration loaded");
        state.status_message = "Connecting to Discord...".to_string();
    }
    tui.draw(&tui_state.read().await.clone())?;

    // Initialize Discord manager
    let mut discord = DiscordManager::new(config.clone());
    
    // Try to connect to Discord (with TUI updates during wait)
    let discord_connected = connect_discord_with_ui(&mut discord, &mut tui, &tui_state).await?;
    
    {
        let mut state = tui_state.write().await;
        state.discord_connected = discord_connected;
        if discord_connected {
            state.log_success("Connected to Discord");
        } else {
            state.log_warn("Discord not connected (will retry)");
        }
        state.status_message = "Starting UDP server...".to_string();
    }
    tui.draw(&tui_state.read().await.clone())?;

    // Initialize usage tracker
    let usage_log_path = config.usage.log_path.as_ref()
        .map(PathBuf::from)
        .unwrap_or_else(UsageTracker::default_log_path);
    let mut usage_tracker = UsageTracker::new(usage_log_path, config.usage.enabled);

    // Load top 3 most played games for display
    {
        let mut state = tui_state.write().await;
        let top_games = usage_tracker.get_top_played(3);
        state.top_played = top_games.into_iter()
            .map(|(title, secs)| (title, format_playtime(secs)))
            .collect();
    }

    // Initialize icon manager with configured mode
    let icon_mode = IconMode::from(config.display.icon_mode.as_str());
    let mut icon_manager = IconManager::new_with_mode(icon_mode);

    // Initialize thumbnail matcher for Discord game icons
    let thumbnail_matcher = Arc::new(ThumbnailMatcher::new());
    
    // Load thumbnail index in background
    {
        let matcher = thumbnail_matcher.clone();
        let tui_state_clone = tui_state.clone();
        tokio::spawn(async move {
            match matcher.load_index().await {
                Ok(count) => {
                    let mut state = tui_state_clone.write().await;
                    state.log_info(&format!("Loaded {} game thumbnails from libretro", count));
                }
                Err(e) => {
                    let mut state = tui_state_clone.write().await;
                    state.log_warn(&format!("Failed to load thumbnails: {}", e));
                }
            }
        });
    }

    // Create server event channel
    let (event_tx, mut event_rx) = mpsc::channel::<ServerEvent>(100);

    // Initialize and start server
    let mut server = Server::new(config.clone(), event_tx);
    if let Err(e) = server.start().await {
        let mut state = tui_state.write().await;
        state.log_error(&format!("Failed to start server: {}", e));
        state.status_message = "Server error! Press Q to quit.".to_string();
        drop(state);
        
        loop {
            let state = tui_state.read().await;
            tui.draw(&state)?;
            if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100), &state)? {
                return Ok(());
            }
        }
    }

    {
        let mut state = tui_state.write().await;
        state.log_success(&format!("Listening on port {}", config.network.listen_port));
        state.status_message = "Waiting for PSP connection...".to_string();
    }
    tui.draw(&tui_state.read().await.clone())?;

    // Create command channel for server
    let server_cmd_tx = server.create_command_channel();

    // Spawn server run loop
    let server_handle = tokio::spawn(async move {
        if let Err(e) = server.run().await {
            error!("Server error: {}", e);
        }
    });

    // Start USB transport if direct mode enabled
    let usb_config = config.usb.clone();
    let (usb_transport_tx, mut usb_transport_rx) = mpsc::channel::<UsbEvent>(100);
    let (usb_cmd_tx, usb_cmd_rx) = mpsc::channel::<UsbCommand>(100);
    let usb_transport_handle = if usb_config.enabled {
        {
            let mut state = tui_state.write().await;
            state.log_info("USB direct mode enabled - waiting for PSP USB device");
        }
        match spawn_usb_task(usb_config, usb_transport_tx, usb_cmd_rx).await {
            Ok(handle) => Some(handle),
            Err(e) => {
                let mut state = tui_state.write().await;
                state.log_error(&format!("Failed to start USB transport: {}", e));
                None
            }
        }
    } else {
        None
    };

    // Main event loop
    let mut refresh_interval = tokio::time::interval(Duration::from_millis(REFRESH_INTERVAL_MS));
    let mut discord_retry_time: Option<Instant> = None;

    loop {
        tokio::select! {
            // Handle server events
            Some(event) = event_rx.recv() => {
                handle_server_event(
                    event,
                    &tui_state,
                    &mut discord,
                    &mut usage_tracker,
                    &mut icon_manager,
                    &server_cmd_tx,
                    &thumbnail_matcher,
                ).await;
            }
            // Handle USB transport events
            Some(usb_event) = usb_transport_rx.recv() => {
                match usb_event {
                    UsbEvent::Connected => {
                        let mut state = tui_state.write().await;
                        state.session_start = Some(Instant::now());
                        state.status_message = "PSP connected via USB!".to_string();
                        state.log_success("PSP connected via USB direct mode");
                    }
                    UsbEvent::Disconnected => {
                        let mut state = tui_state.write().await;
                        state.psp_name = None;
                        state.psp_addr = None;
                        state.current_game = None;
                        state.ascii_art = None;
                        state.session_start = None;
                        state.status_message = "Waiting for PSP USB connection...".to_string();
                        state.log_info("PSP USB disconnected");
                        discord.clear_presence().await.ok();
                    }
                    UsbEvent::GameInfo(usb_game_info) => {
                        // Convert USB game info to protocol GameInfo
                        use crate::protocol::{GameInfo, PspState};
                        let game_info = GameInfo {
                            game_id: usb_game_info.game_id.clone(),
                            title: usb_game_info.title.clone(),
                            state: PspState::try_from(usb_game_info.state).unwrap_or_default(),
                            has_icon: usb_game_info.has_icon,
                            start_time: usb_game_info.start_time as u32,
                            persistent: usb_game_info.persistent,
                            psp_name: usb_game_info.psp_name.clone(),
                        };
                        
                        // Update TUI state
                        {
                            let mut state = tui_state.write().await;
                            if state.session_start.is_none() {
                                state.session_start = Some(Instant::now());
                            }
                            if !game_info.psp_name.is_empty() {
                                state.psp_name = Some(game_info.psp_name.clone());
                            }
                            state.current_game = Some(game_info.clone());
                            state.status_message = format!("Playing: {}", game_info.title);
                            state.log_info(&format!("USB: Now playing {}", game_info.title));
                        }
                        
                        // Update Discord presence
                        let thumbnail_url = thumbnail_matcher.find_thumbnail(&game_info.game_id, &game_info.title).await;
                        discord.update_presence(&game_info, thumbnail_url.as_deref()).await.ok();
                    }
                    UsbEvent::IconData { game_id, data } => {
                        let mut state = tui_state.write().await;
                        state.log_info(&format!("USB: Received icon for {} ({} bytes)", game_id, data.len()));
                        
                        // Convert to ASCII/braille art (same as network mode)
                        if let Some(ascii) = icon_manager.process_icon(&game_id, &data) {
                            // Only update if this is the current game
                            if state.current_game.as_ref().map(|g| &g.game_id) == Some(&game_id) {
                                state.ascii_art = Some(ascii);
                                state.log_success(&format!("Icon ready for {}", game_id));
                            }
                        }
                    }
                    UsbEvent::Error(msg) => {
                        let mut state = tui_state.write().await;
                        state.log_warn(&format!("USB error: {}", msg));
                    }
                    UsbEvent::StatsRequested { psp_name, local_timestamp: _ } => {
                        // PSP requested stats - send our usage data
                        let mut state = tui_state.write().await;
                        let name = if psp_name.is_empty() {
                            state.psp_name.clone().unwrap_or_else(|| "PSP".to_string())
                        } else {
                            psp_name.clone()
                        };
                        state.log_info(&format!("USB: Stats requested by {}", name));
                        
                        // Get usage data for this PSP (returns (json_string, last_updated))
                        let (json_data, last_updated) = usage_tracker.get_usage_for_psp_json(&name);
                        
                        // Send stats response via command channel
                        if let Err(e) = usb_cmd_tx.send(UsbCommand::SendStatsResponse { 
                            last_updated, 
                            json_data: json_data.clone() 
                        }).await {
                            state.log_warn(&format!("USB: Failed to queue stats response: {}", e));
                        } else {
                            state.log_info(&format!("USB: Sending stats ({} bytes, last_updated={})", 
                                                   json_data.len(), last_updated));
                        }
                    }
                    UsbEvent::StatsUploaded { last_updated, json_data } => {
                        // PSP uploaded stats - merge them
                        let mut state = tui_state.write().await;
                        let psp_name = state.psp_name.clone().unwrap_or_else(|| "PSP".to_string());
                        state.log_info(&format!("USB: Stats uploaded (timestamp={}, {} bytes)", 
                                               last_updated, json_data.len()));
                        
                        // Merge the PSP's usage data
                        if let Err(e) = usage_tracker.merge_from_psp(&psp_name, &json_data) {
                            state.log_warn(&format!("USB: Failed to merge stats: {}", e));
                        } else {
                            state.log_success(&format!("USB: Merged stats for {}", psp_name));
                        }
                    }
                }
            }

            // Refresh TUI
            _ = refresh_interval.tick() => {
                // Handle input (pass the state for mouse click detection)
                let state_for_events = tui_state.read().await.clone();
                match tui.handle_events(Duration::from_millis(INPUT_POLL_MS), &state_for_events)? {
                    Some(TuiEvent::Quit) => break,
                    Some(TuiEvent::ShowStats) => {
                        // Load all game stats and play dates, then switch to stats view
                        let all_stats = usage_tracker.get_all_game_stats();
                        let play_dates = usage_tracker.get_all_play_dates();
                        let mut state = tui_state.write().await;
                        state.all_game_stats = all_stats.into_iter()
                            .map(|(title, game_id, seconds, sessions, last_played)| GameStats {
                                title,
                                game_id,
                                total_seconds: seconds,
                                session_count: sessions,
                                last_played,
                            })
                            .collect();
                        state.play_dates = play_dates;
                        state.view_mode = ViewMode::Stats;
                        state.stats_scroll = 0;
                        state.selected_date = None;
                    }
                    Some(TuiEvent::HideStats) => {
                        let mut state = tui_state.write().await;
                        state.view_mode = ViewMode::Main;
                        state.selected_date = None;
                    }
                    Some(TuiEvent::SelectDate(date)) => {
                        let mut state = tui_state.write().await;
                        if let Some(ref d) = date {
                            // Get per-day stats for the selected date
                            state.daily_game_stats = usage_tracker.get_game_stats_for_date(d);
                        } else {
                            state.daily_game_stats = Vec::new();
                        }
                        state.selected_date = date;
                    }
                    None => {}
                }

                // Try Discord reconnection if needed
                if !tui_state.read().await.discord_connected {
                    if let Some(retry_time) = discord_retry_time {
                        if retry_time.elapsed() >= Duration::from_secs(DISCORD_RECONNECT_INTERVAL_SECS) {
                            if discord.connect().await.is_ok() {
                                let mut state = tui_state.write().await;
                                state.discord_connected = true;
                                state.log_success("Reconnected to Discord");
                                discord_retry_time = None;
                            } else {
                                discord_retry_time = Some(Instant::now());
                            }
                        }
                    } else {
                        discord_retry_time = Some(Instant::now());
                    }
                }

                // Draw UI
                let state = tui_state.read().await;
                tui.draw(&state)?;
            }
        }
    }

    // Cleanup
    {
        let mut state = tui_state.write().await;
        state.status_message = "Shutting down...".to_string();
        state.log_info("Shutting down...");
    }
    tui.draw(&tui_state.read().await.clone())?;

    // Flush usage tracker
    usage_tracker.flush_all();

    // Clear Discord presence
    discord.clear_presence().await?;
    discord.disconnect().await?;

    // Abort server
    server_handle.abort();
    
    // Abort USB transport if running
    if let Some(handle) = usb_transport_handle {
        handle.abort();
    }

    Ok(())
}

/// Connect to Discord with TUI updates during retry waits
async fn connect_discord_with_ui(
    discord: &mut DiscordManager,
    tui: &mut Tui,
    tui_state: &Arc<RwLock<TuiState>>,
) -> Result<bool> {
    const MAX_RETRIES: u32 = 3;
    
    for attempt in 1..=MAX_RETRIES {
        {
            let mut state = tui_state.write().await;
            state.status_message = format!("Connecting to Discord (attempt {}/{})", attempt, MAX_RETRIES);
        }
        tui.draw(&tui_state.read().await.clone())?;

        if discord.connect().await.is_ok() {
            return Ok(true);
        }

        if attempt < MAX_RETRIES {
            // Wait with TUI responsiveness
            for _ in 0..50 {
                tokio::time::sleep(Duration::from_millis(100)).await;
                let state = tui_state.read().await;
                if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(10), &state)? {
                    return Err(anyhow::anyhow!("User quit during Discord connection"));
                }
                tui.draw(&state)?;
            }
        }
    }

    Ok(false)
}

/// Handle a server event
async fn handle_server_event(
    event: ServerEvent,
    tui_state: &Arc<RwLock<TuiState>>,
    discord: &mut DiscordManager,
    usage_tracker: &mut UsageTracker,
    icon_manager: &mut IconManager,
    server_cmd_tx: &mpsc::Sender<ServerCommand>,
    thumbnail_matcher: &Arc<ThumbnailMatcher>,
) {
    match event {
        ServerEvent::PspConnected { addr, name, battery } => {
            let mut state = tui_state.write().await;
            state.psp_name = Some(name.clone());
            state.psp_addr = Some(addr);
            state.battery_level = battery;
            state.session_start = Some(Instant::now());
            state.status_message = format!("Connected to {}", name);
            state.log_success(&format!("PSP '{}' connected from {}", name, addr));
            
            usage_tracker.register_psp(addr, name.clone());
            
            // Set idle presence
            if let Err(e) = discord.set_idle_presence(&name).await {
                state.log_warn(&format!("Discord error: {}", e));
            }
        }

        ServerEvent::PspDisconnected { addr, name } => {
            let mut state = tui_state.write().await;
            
            // Only process if this is our current PSP
            let is_current_psp = state.psp_addr.map(|a| a == addr).unwrap_or(false);
            
            if is_current_psp {
                state.psp_name = None;
                state.psp_addr = None;
                state.current_game = None;
                state.ascii_art = None;
                state.session_start = None;
                state.status_message = "Waiting for PSP connection...".to_string();
                state.log_info(&format!("PSP '{}' disconnected", name));
                
                usage_tracker.unregister_psp(addr);
                
                if let Err(e) = discord.clear_presence().await {
                    state.log_warn(&format!("Discord error: {}", e));
                }
            } else {
                // Log but don't clear state for unknown connections
                state.log_info(&format!("Unknown device '{}' disconnected", name));
            }
        }

        ServerEvent::GameInfoUpdated { addr, info } => {
            let mut state = tui_state.write().await;
            
            // Start session if not already started (handles direct connections without discovery)
            if state.session_start.is_none() {
                state.session_start = Some(Instant::now());
                state.psp_addr = Some(addr);
            }
            
            // Check if game changed
            let game_changed = state.current_game.as_ref()
                .map(|g| g.game_id != info.game_id)
                .unwrap_or(true);

            // Update PSP name if provided
            if !info.psp_name.is_empty() && state.psp_name.as_deref() != Some(&info.psp_name) {
                state.psp_name = Some(info.psp_name.clone());
            }

            if game_changed {
                if !info.title.is_empty() {
                    state.log_info(&format!("Now playing: {}", info.title));
                } else {
                    state.log_info(&format!("State: {}", info.state.as_str()));
                }
            }

            // Check if we have the icon cached - request on ANY update if missing
            if !icon_manager.has_ascii(&info.game_id) {
                state.ascii_art = None;
                // Request icon from PSP (retry on every update until we get it)
                if !info.game_id.is_empty() && info.game_id != "XMB" && info.game_id != "UNK" {
                    // Only log on first request (game change)
                    if game_changed {
                        state.log_info(&format!("Requesting icon for {}", info.game_id));
                    }
                    let _ = server_cmd_tx.send(ServerCommand::RequestIcon { 
                        addr, 
                        game_id: info.game_id.clone() 
                    }).await;
                }
            } else if state.ascii_art.is_none() {
                // We have it cached but haven't set it yet
                state.ascii_art = icon_manager.get_ascii(&info.game_id).cloned();
            }

            state.current_game = Some(info.clone());
            state.status_message = format!("Playing: {}", 
                if info.title.is_empty() { info.state.as_str().to_string() } else { info.title.clone() });
            
            // Update usage tracker
            usage_tracker.update_game(addr, &info);
            
            // Update top played games if game changed
            if game_changed {
                let top_games = usage_tracker.get_top_played(3);
                state.top_played = top_games.into_iter()
                    .map(|(title, secs)| (title, format_playtime(secs)))
                    .collect();
            }
            
            // Update Discord presence with thumbnail lookup
            let thumbnail_url = thumbnail_matcher.find_thumbnail(&info.game_id, &info.title).await;
            if let Some(ref _url) = thumbnail_url {
                if game_changed {
                    state.log_info(&format!("Found thumbnail for {}", info.title));
                }
            }
            drop(state); // Release lock before async Discord call
            
            if let Err(e) = discord.update_presence(&info, thumbnail_url.as_deref()).await {
                let mut state = tui_state.write().await;
                state.log_warn(&format!("Discord error: {}", e));
            }
        }

        ServerEvent::HeartbeatReceived { addr: _, heartbeat } => {
            // Don't log heartbeats (too frequent), just update WiFi strength
            let mut state = tui_state.write().await;
            state.wifi_strength = heartbeat.wifi_strength;
        }

        ServerEvent::IconReceived { game_id, data } => {
            let mut state = tui_state.write().await;
            state.log_info(&format!("Received icon for {} ({} bytes)", game_id, data.len()));
            
            // Convert to ASCII art asynchronously
            // For now, do it inline (could be spawned for larger icons)
            if let Some(ascii) = icon_manager.process_icon(&game_id, &data) {
                // Only update if this is the current game
                if state.current_game.as_ref().map(|g| &g.game_id) == Some(&game_id) {
                    state.ascii_art = Some(ascii);
                    state.log_success(&format!("Icon ready for {}", game_id));
                }
            }
        }

        ServerEvent::StatsRequested { addr, psp_name } => {
            let mut state = tui_state.write().await;
            state.log_info(&format!("Stats requested from {} ({})", psp_name, addr));
            
            // Get usage data for this PSP using the sync API
            let (json_data, last_updated) = usage_tracker.get_usage_for_psp_json(&psp_name);
            
            state.log_info(&format!("Sending {} bytes to PSP (last_updated: {})", 
                json_data.len(), last_updated));
            
            // Send stats via server command
            let _ = server_cmd_tx.send(ServerCommand::SendStats {
                addr,
                json_data: json_data.into_bytes(),
                last_updated,
            }).await;
        }

        ServerEvent::StatsUploaded { addr, psp_name, last_updated, json_data } => {
            let mut state = tui_state.write().await;
            state.log_info(&format!("Stats uploaded from {} ({}), last_updated: {}", psp_name, addr, last_updated));
            
            // Merge the uploaded data into our usage tracker
            match usage_tracker.merge_from_psp(&psp_name, &json_data) {
                Ok(merge_count) => {
                    state.log_success(&format!("Merged {} games from PSP '{}'", merge_count, psp_name));
                    
                    // Update top played games after merge
                    let top_games = usage_tracker.get_top_played(3);
                    state.top_played = top_games.into_iter()
                        .map(|(title, secs)| (title, format_playtime(secs)))
                        .collect();
                }
                Err(e) => {
                    state.log_warn(&format!("Failed to merge stats from {}: {}", psp_name, e));
                }
            }
        }
    }
}

/// Format playtime in seconds to human-readable string
fn format_playtime(total_secs: u64) -> String {
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;

    if hours > 0 {
        format!("{}h {}m", hours, mins)
    } else if mins > 0 {
        format!("{}m", mins)
    } else {
        "< 1m".to_string()
    }
}
