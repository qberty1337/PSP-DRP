//! ASCII Art Module
//!
//! Converts game icons to ASCII art or Braille art for terminal display.
//! Supports two modes:
//! - ASCII: Uses the `artem` crate for image-to-ASCII conversion
//! - Braille: Uses Unicode Braille patterns for higher resolution (4x vertical density)

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::num::NonZeroU32;
use std::path::PathBuf;

use artem::config::{ConfigBuilder, TargetType};
use image::{DynamicImage, GenericImageView, Luma};
use tracing::{debug, error, info, warn};

/// Target width for ASCII art (in characters)
const ASCII_WIDTH: u32 = 35;

/// Target width for Braille art (in characters)
/// Each Braille character represents 2 horizontal pixels
const BRAILLE_WIDTH: u32 = 40;

/// Icon rendering mode
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum IconMode {
    Ascii,
    Braille,
}

impl From<&str> for IconMode {
    fn from(s: &str) -> Self {
        match s.to_lowercase().as_str() {
            "braille" => IconMode::Braille,
            _ => IconMode::Ascii,
        }
    }
}

/// Manager for icon storage and ASCII/Braille conversion
pub struct IconManager {
    /// Directory for temporary icon storage
    temp_dir: PathBuf,
    /// Cache of converted art (game_id -> art_string)
    art_cache: HashMap<String, String>,
    /// Current game icon path (for cleanup)
    current_icon_path: Option<PathBuf>,
    /// Rendering mode
    mode: IconMode,
}

impl IconManager {
    /// Create a new IconManager with the specified mode
    pub fn new_with_mode(mode: IconMode) -> Self {
        let temp_dir = Self::get_temp_dir();
        
        // Create temp directory if it doesn't exist
        if let Err(e) = fs::create_dir_all(&temp_dir) {
            warn!("Failed to create temp icon directory: {}", e);
        } else {
            debug!("Icon temp directory: {}", temp_dir.display());
        }

        info!("IconManager using {:?} mode", mode);

        Self {
            temp_dir,
            art_cache: HashMap::new(),
            current_icon_path: None,
            mode,
        }
    }

    /// Create a new IconManager with default ASCII mode
    pub fn new() -> Self {
        Self::new_with_mode(IconMode::Ascii)
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

    /// Check if we already have art for a game
    pub fn has_ascii(&self, game_id: &str) -> bool {
        self.art_cache.contains_key(game_id)
    }

    /// Get cached art for a game
    pub fn get_ascii(&self, game_id: &str) -> Option<&String> {
        self.art_cache.get(game_id)
    }

    /// Store and convert icon data to art
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

        // Convert based on mode
        let result = match self.mode {
            IconMode::Ascii => self.convert_to_ascii(&icon_path),
            IconMode::Braille => self.convert_to_braille(&icon_path),
        };

        match result {
            Some(art) => {
                info!("Generated {:?} art for {} ({} chars)", self.mode, game_id, art.len());
                self.art_cache.insert(game_id.to_string(), art.clone());
                Some(art)
            }
            None => {
                warn!("Failed to convert icon for {}", game_id);
                // Generate placeholder instead
                let placeholder = self.generate_placeholder(game_id, "Unknown Game");
                self.art_cache.insert(game_id.to_string(), placeholder.clone());
                Some(placeholder)
            }
        }
    }

    /// Convert an image file to ASCII art using artem
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
        // - invert: makes white/bright areas become spaces (negative space)
        // - File target: no ANSI color codes
        let config = ConfigBuilder::new()
            .target_size(NonZeroU32::new(ASCII_WIDTH).unwrap())
            .target(TargetType::File)
            .invert(true)  // White background becomes spaces
            .border(false)
            .build();

        // Convert to ASCII
        match artem::convert(img, &config) {
            ascii => Some(ascii)
        }
    }

    /// Convert an image file to Braille art
    /// Each Braille character represents a 2x4 grid of dots
    fn convert_to_braille(&self, icon_path: &PathBuf) -> Option<String> {
        // Load and process the image
        let img = match image::open(icon_path) {
            Ok(img) => img,
            Err(e) => {
                error!("Failed to load icon image: {}", e);
                return None;
            }
        };

        // Log source dimensions
        let (orig_w, orig_h) = img.dimensions();
        info!("Icon source dimensions: {}x{}", orig_w, orig_h);

        // Resize to target width (height is proportional, but we need multiples of 4)
        let (orig_w, orig_h) = img.dimensions();
        let target_w = BRAILLE_WIDTH * 2; // 2 pixels per Braille char width
        
        // Calculate height maintaining aspect ratio
        let target_h = (orig_h as f32 / orig_w as f32 * target_w as f32) as u32;
        // Round up to multiple of 4 for Braille rows
        let target_h = ((target_h + 3) / 4) * 4;

        let img = img.resize_exact(
            target_w,
            target_h,
            image::imageops::FilterType::Lanczos3,
        );

        // Convert to grayscale
        let gray = img.to_luma8();

        // Calculate threshold using Otsu's method or simple mean
        let threshold = calculate_threshold(&gray);

        // Convert to Braille
        let braille = image_to_braille(&gray, threshold, true); // invert for dark background

        Some(braille)
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
        self.art_cache.clear();
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

/// Calculate threshold for binary conversion using simple mean
fn calculate_threshold(img: &image::GrayImage) -> u8 {
    let sum: u64 = img.pixels().map(|p| p.0[0] as u64).sum();
    let count = img.width() as u64 * img.height() as u64;
    (sum / count) as u8
}

/// Convert a grayscale image to Braille art string
fn image_to_braille(img: &image::GrayImage, threshold: u8, invert: bool) -> String {
    let (width, height) = img.dimensions();
    let mut result = String::new();

    // Braille dot positions (in a 2x4 grid):
    // 1 4
    // 2 5
    // 3 6
    // 7 8
    // Dot values: dot 1=1, dot 2=2, dot 3=4, dot 4=8, dot 5=16, dot 6=32, dot 7=64, dot 8=128

    for y in (0..height).step_by(4) {
        for x in (0..width).step_by(2) {
            let mut pattern: u8 = 0;

            // Check each dot position
            let dots = [
                (0, 0, 0x01), // dot 1
                (0, 1, 0x02), // dot 2
                (0, 2, 0x04), // dot 3
                (1, 0, 0x08), // dot 4
                (1, 1, 0x10), // dot 5
                (1, 2, 0x20), // dot 6
                (0, 3, 0x40), // dot 7
                (1, 3, 0x80), // dot 8
            ];

            for (dx, dy, value) in dots {
                let px = x + dx;
                let py = y + dy;
                
                if px < width && py < height {
                    let pixel = img.get_pixel(px, py).0[0];
                    let is_dark = if invert {
                        pixel > threshold  // Invert: bright pixels become dots
                    } else {
                        pixel < threshold  // Normal: dark pixels become dots
                    };
                    
                    if is_dark {
                        pattern |= value;
                    }
                }
            }

            // Braille Unicode block starts at U+2800
            let braille_char = char::from_u32(0x2800 + pattern as u32).unwrap_or(' ');
            result.push(braille_char);
        }
        result.push('\n');
    }

    result
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
