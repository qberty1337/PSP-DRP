//! PSP Discord Rich Presence - Desktop Companion
//!
//! A fancy TUI application that receives data from a PSP running the
//! PSP DRP plugin and updates Discord Rich Presence accordingly.

mod ascii_art;
mod config;
mod discord;
mod protocol;
mod server;
mod tui;
mod usage_tracker;

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
use tui::{Tui, TuiEvent, TuiState};
use usage_tracker::UsageTracker;

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
                drop(state);
                
                if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100))? {
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
            drop(state);
            
            if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100))? {
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
            drop(state);
            
            if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(100))? {
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
                ).await;
            }

            // Refresh TUI
            _ = refresh_interval.tick() => {
                // Handle input
                if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(INPUT_POLL_MS))? {
                    break;
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
                if let Some(TuiEvent::Quit) = tui.handle_events(Duration::from_millis(10))? {
                    return Err(anyhow::anyhow!("User quit during Discord connection"));
                }
                let state = tui_state.read().await;
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

        ServerEvent::PspDisconnected { addr } => {
            let mut state = tui_state.write().await;
            let name = state.psp_name.clone().unwrap_or_else(|| "PSP".to_string());
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
            
            // Update Discord presence
            if let Err(e) = discord.update_presence(&info).await {
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
