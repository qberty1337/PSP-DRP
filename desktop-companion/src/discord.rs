use anyhow::Result;
use discord_rich_presence::{activity, DiscordIpc, DiscordIpcClient};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::Mutex;
use tracing::{debug, error, info, warn};

use crate::config::Config;
use crate::protocol::{GameInfo, PspState};

/// Discord Rich Presence manager
pub struct DiscordManager {
    client: Arc<Mutex<Option<DiscordIpcClient>>>,
    config: Config,
    connected: bool,
    last_game_id: Option<String>,
    last_start_ts: Option<i64>,
}

impl DiscordManager {
    pub fn new(config: Config) -> Self {
        Self {
            client: Arc::new(Mutex::new(None)),
            config,
            connected: false,
            last_game_id: None,
            last_start_ts: None,
        }
    }
    
    /// Connect to Discord
    pub async fn connect(&mut self) -> Result<()> {
        let client_id = self.config.discord.client_id.clone();
        
        info!("Connecting to Discord with client ID: {}...", &client_id[..8.min(client_id.len())]);
        
        let mut client = match DiscordIpcClient::new(&client_id) {
            Ok(c) => c,
            Err(e) => {
                error!("Failed to create Discord client: {}", e);
                return Err(anyhow::anyhow!("Failed to create Discord client: {}", e));
            }
        };
        
        match client.connect() {
            Ok(_) => {
                info!("Connected to Discord successfully");
                self.connected = true;
                *self.client.lock().await = Some(client);
                Ok(())
            }
            Err(e) => {
                error!("Failed to connect to Discord: {}", e);
                Err(anyhow::anyhow!("Failed to connect to Discord: {}", e))
            }
        }
    }
    
    /// Disconnect from Discord
    pub async fn disconnect(&mut self) -> Result<()> {
        if let Some(mut client) = self.client.lock().await.take() {
            if let Err(e) = client.close() {
                warn!("Error closing Discord connection: {}", e);
            }
            self.connected = false;
            info!("Disconnected from Discord");
        }
        Ok(())
    }
    
    /// Update presence with game info
    /// If thumbnail_url is provided, it will be used as the large image
    pub async fn update_presence(&mut self, game_info: &GameInfo, thumbnail_url: Option<&str>) -> Result<()> {
        let mut guard = self.client.lock().await;
        let client = guard.as_mut().ok_or_else(|| anyhow::anyhow!("Not connected to Discord"))?;
        
        // Build the activity
        let details = if game_info.title.is_empty() {
            game_info.state.as_str().to_string()
        } else {
            game_info.title.clone()
        };
        
        let state_text = match game_info.state {
            PspState::Xmb => "Browsing XMB".to_string(),
            _ => self.config.discord.state_text.clone(),
        };
        
        let mut activity_builder = activity::Activity::new()
            .details(&details)
            .state(&state_text);
        
        // Add timestamps if enabled
        if self.config.discord.show_elapsed_time {
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_secs() as i64)
                .unwrap_or(0);

            if self.last_game_id.as_deref() != Some(&game_info.game_id) || self.last_start_ts.is_none() {
                self.last_game_id = Some(game_info.game_id.clone());
                self.last_start_ts = Some(now);
            }

            if let Some(ts) = self.last_start_ts {
                if ts > 0 {
                    activity_builder = activity_builder.timestamps(
                        activity::Timestamps::new().start(ts)
                    );
                }
            }
        }
        
        // Add assets - use thumbnail URL if provided, otherwise fall back to static asset
        let large_image = thumbnail_url.unwrap_or("psp_logo");
        let large_text = if thumbnail_url.is_some() {
            game_info.title.as_str()
        } else {
            "PlayStation Portable"
        };
        
        let assets = activity::Assets::new()
            .large_image(large_image)
            .large_text(large_text)
            .small_image("psp_logo")
            .small_text("PlayStation Portable");
        
        activity_builder = activity_builder.assets(assets);
        
        // Set the activity
        match client.set_activity(activity_builder) {
            Ok(_) => {
                debug!("Updated Discord presence: {}", details);
                Ok(())
            }
            Err(e) => {
                warn!("Failed to update presence: {}", e);
                Err(anyhow::anyhow!("Failed to update presence: {}", e))
            }
        }
    }
    
    /// Clear presence (when PSP disconnects)
    pub async fn clear_presence(&mut self) -> Result<()> {
        let mut guard = self.client.lock().await;
        if let Some(client) = guard.as_mut() {
            if let Err(e) = client.clear_activity() {
                warn!("Failed to clear presence: {}", e);
            }
            debug!("Cleared Discord presence");
        }
        self.last_game_id = None;
        self.last_start_ts = None;
        Ok(())
    }
    
    /// Update presence to show PSP connected but idle
    pub async fn set_idle_presence(&mut self, psp_name: &str) -> Result<()> {
        let mut guard = self.client.lock().await;
        let client = guard.as_mut().ok_or_else(|| anyhow::anyhow!("Not connected to Discord"))?;
        
        let state_text = format!("on {}", psp_name);
        let activity = activity::Activity::new()
            .details("Browsing XMB")
            .state(&state_text)
            .assets(
                activity::Assets::new()
                    .large_image("psp_logo")
                    .large_text("PlayStation Portable")
            );
        
        match client.set_activity(activity) {
            Ok(_) => {
                debug!("Set idle presence for {}", psp_name);
                Ok(())
            }
            Err(e) => {
                warn!("Failed to set idle presence: {}", e);
                Err(anyhow::anyhow!("Failed to set idle presence: {}", e))
            }
        }
    }
}

impl Drop for DiscordManager {
    fn drop(&mut self) {
        // Try to close the connection on drop
        if let Ok(mut guard) = self.client.try_lock() {
            if let Some(mut client) = guard.take() {
                let _ = client.close();
            }
        }
    }
}
