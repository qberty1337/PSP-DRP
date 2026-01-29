//! ASCII Art Module
//!
//! Converts game icons to ASCII art for terminal display.
//! Uses the `artem` crate for image-to-ASCII conversion.

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::num::NonZeroU32;
use std::path::PathBuf;

use artem::ConfigBuilder;
use tracing::{debug, error, info, warn};

/// Target width for ASCII art (in characters)
const ASCII_WIDTH: u32 = 40;

/// Manager for icon storage and ASCII conversion
pub struct IconManager {
    /// Directory for temporary icon storage
    temp_dir: PathBuf,
    /// Cache of converted ASCII art (game_id -> ascii_string)
    ascii_cache: HashMap<String, String>,
    /// Current game icon path (for cleanup)
    current_icon_path: Option<PathBuf>,
}

impl IconManager {
    /// Create a new IconManager
    pub fn new() -> Self {
        let temp_dir = Self::get_temp_dir();
        
        // Create temp directory if it doesn't exist
        if let Err(e) = fs::create_dir_all(&temp_dir) {
            warn!("Failed to create temp icon directory: {}", e);
        } else {
            debug!("Icon temp directory: {}", temp_dir.display());
        }

        Self {
            temp_dir,
            ascii_cache: HashMap::new(),
            current_icon_path: None,
        }
    }

    /// Get the temp directory path (beside the executable)
    fn get_temp_dir() -> PathBuf {
        if let Ok(exe_path) = std::env::current_exe() {
            if let Some(exe_dir) = exe_path.parent() {
                return exe_dir.join("temp_icons");
            }
        }
        PathBuf::from("temp_icons")
    }

    /// Check if we already have ASCII art for a game
    pub fn has_ascii(&self, game_id: &str) -> bool {
        self.ascii_cache.contains_key(game_id)
    }

    /// Get cached ASCII art for a game
    pub fn get_ascii(&self, game_id: &str) -> Option<&String> {
        self.ascii_cache.get(game_id)
    }

    /// Store and convert icon data to ASCII art
    pub fn process_icon(&mut self, game_id: &str, data: &[u8]) -> Option<String> {
        // Save icon to temp file
        let icon_path = self.temp_dir.join(format!("{}.png", game_id));
        
        match File::create(&icon_path) {
            Ok(mut file) => {
                if let Err(e) = file.write_all(data) {
                    error!("Failed to write icon file: {}", e);
                    return None;
                }
            }
            Err(e) => {
                error!("Failed to create icon file: {}", e);
                return None;
            }
        }

        // Store current path for cleanup
        self.current_icon_path = Some(icon_path.clone());

        // Convert to ASCII
        match self.convert_to_ascii(&icon_path) {
            Some(ascii) => {
                info!("Generated ASCII art for {} ({} chars)", game_id, ascii.len());
                self.ascii_cache.insert(game_id.to_string(), ascii.clone());
                Some(ascii)
            }
            None => {
                warn!("Failed to convert icon to ASCII for {}", game_id);
                // Generate placeholder instead
                let placeholder = self.generate_placeholder(game_id, "Unknown Game");
                self.ascii_cache.insert(game_id.to_string(), placeholder.clone());
                Some(placeholder)
            }
        }
    }

    /// Convert an image file to ASCII art
    fn convert_to_ascii(&self, icon_path: &PathBuf) -> Option<String> {
        // Load the image
        let img = match image::open(icon_path) {
            Ok(img) => img,
            Err(e) => {
                error!("Failed to load icon image: {}", e);
                return None;
            }
        };

        // Configure artem for conversion
        let config = ConfigBuilder::new()
            .target_size(NonZeroU32::new(ASCII_WIDTH).unwrap())
            .border(false)
            .build();

        // Convert to ASCII
        match artem::convert(img, &config) {
            ascii => Some(ascii)
        }
    }

    /// Generate a placeholder ASCII art for games without icons
    pub fn generate_placeholder(&self, game_id: &str, title: &str) -> String {
        let title_display = if title.len() > 20 {
            format!("{}...", &title[..17])
        } else {
            title.to_string()
        };

        let id_display = if game_id.len() > 12 {
            format!("{}...", &game_id[..9])
        } else {
            game_id.to_string()
        };

        // PSP UMD case ASCII art placeholder
        format!(
r#"╔══════════════════════════════════════╗
║                                      ║
║     ╭────────────────────────╮       ║
║     │   ╔════════════════╗   │       ║
║     │   ║                ║   │       ║
║     │   ║    PSP GAME    ║   │       ║
║     │   ║                ║   │       ║
║     │   ╚════════════════╝   │       ║
║     │                        │       ║
║     │  {:^20}  │       ║
║     │                        │       ║
║     │  ID: {:^12}      │       ║
║     │                        │       ║
║     ╰────────────────────────╯       ║
║                                      ║
╚══════════════════════════════════════╝"#,
            title_display, id_display
        )
    }

    /// Clear the cache and remove temp files
    #[allow(dead_code)]
    pub fn clear(&mut self) {
        self.ascii_cache.clear();
        self.cleanup_temp_files();
    }

    /// Remove temporary icon files
    fn cleanup_temp_files(&self) {
        if self.temp_dir.exists() {
            if let Ok(entries) = fs::read_dir(&self.temp_dir) {
                for entry in entries.flatten() {
                    if let Err(e) = fs::remove_file(entry.path()) {
                        debug!("Failed to remove temp file: {}", e);
                    }
                }
            }
            // Try to remove the directory itself
            let _ = fs::remove_dir(&self.temp_dir);
        }
    }
}

impl Default for IconManager {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for IconManager {
    fn drop(&mut self) {
        // Clean up temp files on shutdown
        self.cleanup_temp_files();
        debug!("IconManager cleaned up temp files");
    }
}

/// Split ASCII art into lines for rendering
#[allow(dead_code)]
pub fn ascii_to_lines(ascii: &str) -> Vec<&str> {
    ascii.lines().collect()
}

/// Get the dimensions of ASCII art
#[allow(dead_code)]
pub fn ascii_dimensions(ascii: &str) -> (usize, usize) {
    let lines: Vec<&str> = ascii.lines().collect();
    let height = lines.len();
    let width = lines.iter().map(|l| l.chars().count()).max().unwrap_or(0);
    (width, height)
}
