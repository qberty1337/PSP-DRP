use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// Application configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// Discord application settings
    pub discord: DiscordConfig,

    /// Network settings
    pub network: NetworkConfig,

    /// Display settings
    pub display: DisplayConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscordConfig {
    /// Discord Application Client ID (required)
    /// Get this from https://discord.com/developers/applications
    pub client_id: String,

    /// Whether to show elapsed time in presence
    #[serde(default = "default_true")]
    pub show_elapsed_time: bool,

    /// Custom state text (shown below game title)
    #[serde(default = "default_state_text")]
    pub state_text: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkConfig {
    /// Port to listen for PSP connections
    #[serde(default = "default_listen_port")]
    pub listen_port: u16,

    /// Port for auto-discovery broadcasts
    #[serde(default = "default_discovery_port")]
    pub discovery_port: u16,

    /// Enable auto-discovery of PSPs on network
    #[serde(default = "default_true")]
    pub auto_discovery: bool,

    /// How often to send discovery broadcasts (seconds)
    #[serde(default = "default_discovery_interval")]
    pub discovery_interval: u64,

    /// Timeout before considering PSP disconnected (seconds)
    #[serde(default = "default_timeout")]
    pub timeout_seconds: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DisplayConfig {
    /// Show system tray icon
    #[serde(default = "default_true")]
    pub show_tray_icon: bool,

    /// Log level (trace, debug, info, warn, error)
    #[serde(default = "default_log_level")]
    pub log_level: String,
}

// Default value functions
fn default_true() -> bool {
    true
}
fn default_state_text() -> String {
    "Playing on PSP".to_string()
}
fn default_listen_port() -> u16 {
    9276
}
fn default_discovery_port() -> u16 {
    9277
}
fn default_discovery_interval() -> u64 {
    30
}
fn default_timeout() -> u64 {
    90
}
fn default_log_level() -> String {
    "info".to_string()
}

impl Default for Config {
    fn default() -> Self {
        Self {
            discord: DiscordConfig {
                client_id: String::new(), // Must be set by user
                show_elapsed_time: true,
                state_text: default_state_text(),
            },
            network: NetworkConfig {
                listen_port: default_listen_port(),
                discovery_port: default_discovery_port(),
                auto_discovery: true,
                discovery_interval: default_discovery_interval(),
                timeout_seconds: default_timeout(),
            },
            display: DisplayConfig {
                show_tray_icon: true,
                log_level: default_log_level(),
            },
        }
    }
}

impl Config {
    /// Get the config file path
    pub fn config_path() -> PathBuf {
        if let Ok(exe_path) = std::env::current_exe() {
            if let Some(exe_dir) = exe_path.parent() {
                return exe_dir.join("config.toml");
            }
        }
        PathBuf::from("config.toml")
    }

    /// Load config from file, creating default if it doesn't exist
    pub fn load() -> anyhow::Result<Self> {
        let path = Self::config_path();

        if path.exists() {
            let content = std::fs::read_to_string(&path)?;
            let config: Config = toml::from_str(&content)?;
            Ok(config)
        } else {
            // Create default config
            let config = Config::default();
            config.save()?;
            Ok(config)
        }
    }

    /// Save config to file
    pub fn save(&self) -> anyhow::Result<()> {
        let path = Self::config_path();

        // Ensure parent directory exists
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }

        let content = toml::to_string_pretty(self)?;
        std::fs::write(&path, content)?;

        Ok(())
    }

    /// Validate the configuration
    pub fn validate(&self) -> anyhow::Result<()> {
        if self.discord.client_id.is_empty() {
            anyhow::bail!(
                "Discord client_id is not set!\n\
                Please edit the config file at: {}\n\
                \n\
                To get a client ID:\n\
                1. Go to https://discord.com/developers/applications\n\
                2. Click 'New Application'\n\
                3. Copy the 'Application ID' (this is your client_id)\n\
                4. Paste it into the config file",
                Self::config_path().display()
            );
        }

        Ok(())
    }
}
