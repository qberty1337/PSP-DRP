mod config;
mod discord;
mod protocol;
mod server;

use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use tokio::sync::mpsc;
use tracing::{error, info, warn, Level};
use tracing_subscriber::FmtSubscriber;

use config::Config;
use discord::DiscordManager;
use protocol::VERSION;
use server::{Server, ServerEvent};

#[tokio::main]
async fn main() -> Result<()> {
    // Load configuration
    let config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("Failed to load config: {}", e);
            eprintln!("Config file location: {}", Config::config_path().display());
            return Err(e);
        }
    };
    
    // Set up logging
    let log_level = match config.display.log_level.to_lowercase().as_str() {
        "trace" => Level::TRACE,
        "debug" => Level::DEBUG,
        "info" => Level::INFO,
        "warn" => Level::WARN,
        "error" => Level::ERROR,
        _ => Level::INFO,
    };
    
    let subscriber = FmtSubscriber::builder()
        .with_max_level(log_level)
        .with_target(false)
        .finish();
    tracing::subscriber::set_global_default(subscriber)?;
    
    info!("PSP Discord Rich Presence v{}", env!("CARGO_PKG_VERSION"));
    info!("Protocol version: {}", VERSION);
    info!("Config file: {}", Config::config_path().display());
    
    // Validate configuration
    if let Err(e) = config.validate() {
        error!("{}", e);
        return Err(e);
    }
    
    // Create event channel
    let (event_tx, mut event_rx) = mpsc::channel::<ServerEvent>(100);
    
    // Create Discord manager
    let mut discord = DiscordManager::new(config.clone());
    
    // Connect to Discord
    loop {
        match discord.connect().await {
            Ok(_) => break,
            Err(e) => {
                warn!("Failed to connect to Discord: {}. Retrying in 5 seconds...", e);
                tokio::time::sleep(Duration::from_secs(5)).await;
            }
        }
    }
    
    // Create and start server
    let mut server = Server::new(config.clone(), event_tx);
    server.start().await?;
    let server = Arc::new(server);
    
    // Spawn server receive loop
    let server_run = Arc::clone(&server);
    let server_handle = tokio::spawn(async move {
        if let Err(e) = server_run.run().await {
            error!("Server error: {}", e);
        }
    });
    
    // Spawn discovery broadcast loop if enabled
    if config.network.auto_discovery {
        let discovery_interval = config.network.discovery_interval;
        let discovery_server = Arc::clone(&server);
        info!("Auto-discovery enabled, broadcasting every {} seconds", discovery_interval);
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(discovery_interval));
            loop {
                interval.tick().await;
                if let Err(e) = discovery_server.send_discovery_broadcast().await {
                    tracing::debug!("Discovery broadcast failed: {}", e);
                }
            }
        });
    }
    
    info!("Waiting for PSP connections on port {}...", config.network.listen_port);
    
    // Main event loop
    loop {
        tokio::select! {
            Some(event) = event_rx.recv() => {
                match event {
                    ServerEvent::PspConnected { addr, name, battery } => {
                        info!("PSP connected: {} ({}) - Battery: {}%", name, addr, battery);
                        if let Err(e) = discord.set_idle_presence(&name).await {
                            warn!("Failed to set idle presence: {}", e);
                        }
                    }
                    
                    ServerEvent::PspDisconnected { addr } => {
                        info!("PSP disconnected: {}", addr);
                        if let Err(e) = discord.clear_presence().await {
                            warn!("Failed to clear presence: {}", e);
                        }
                    }
                    
                    ServerEvent::GameInfoUpdated { addr, info } => {
                        info!("Game update from {}: {} - {}", addr, info.game_id, info.title);
                        if let Err(e) = discord.update_presence(&info).await {
                            warn!("Failed to update presence: {}", e);
                        }
                    }
                    
                    ServerEvent::HeartbeatReceived { addr, heartbeat } => {
                        tracing::trace!(
                            "Heartbeat from {}: uptime={}s, wifi={}%",
                            addr, heartbeat.uptime_seconds, heartbeat.wifi_strength
                        );
                    }
                    
                    ServerEvent::IconReceived { game_id, data } => {
                        info!("Received icon for game {}: {} bytes", game_id, data.len());
                        // Save icon beside the executable for inspection
                        let icon_filename = format!("icon_{}.png", game_id);
                        let icon_path = match std::env::current_exe()
                            .ok()
                            .and_then(|p| p.parent().map(|dir| dir.join(&icon_filename)))
                        {
                            Some(path) => path,
                            None => std::path::PathBuf::from(&icon_filename),
                        };

                        if let Err(e) = std::fs::write(&icon_path, &data) {
                            warn!("Failed to save icon: {}", e);
                        } else {
                            info!("Saved icon to {}", icon_path.display());
                        }
                    }
                }
            }
            
            _ = tokio::signal::ctrl_c() => {
                info!("Shutting down...");
                if let Err(e) = discord.clear_presence().await {
                    warn!("Failed to clear presence: {}", e);
                }
                discord.disconnect().await?;
                break;
            }
        }
    }
    
    server_handle.abort();
    Ok(())
}
