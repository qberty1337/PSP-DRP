//! Thumbnail matcher for PSP games using libretro-thumbnails
//!
//! This module fetches the list of available PSP thumbnails from the libretro
//! thumbnails repository and uses fuzzy matching to find the best match for
//! a given game title. It supports multiple thumbnail sources with fallback.

use anyhow::Result;
use std::collections::HashMap;
use std::sync::Arc;
use strsim::jaro_winkler;
use tokio::sync::RwLock;
use tracing::{debug, info, warn};

/// Thumbnail source configuration
struct ThumbnailSource {
    /// Display name for logging
    name: &'static str,
    /// URL for the HTML directory listing
    index_url: &'static str,
    /// Base URL for raw image access via GitHub
    raw_base_url: &'static str,
}

/// Available thumbnail sources in priority order (boxarts first, then snaps)
const THUMBNAIL_SOURCES: &[ThumbnailSource] = &[
    ThumbnailSource {
        name: "Boxarts",
        index_url: "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/",
        raw_base_url: "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_Portable/master/Named_Boxarts/",
    },
    ThumbnailSource {
        name: "Snaps",
        index_url: "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Snaps/",
        raw_base_url: "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_Portable/master/Named_Snaps/",
    },
];

/// Minimum similarity threshold for a match (0.0 to 1.0)
const MIN_SIMILARITY_THRESHOLD: f64 = 0.6;

/// Cache for matched thumbnails (game_id -> thumbnail URL)
type ThumbnailCache = HashMap<String, Option<String>>;

/// A loaded thumbnail index with its source info
struct LoadedIndex {
    /// Thumbnail filenames (without .png extension)
    thumbnails: Vec<String>,
    /// Base URL for constructing raw image URLs
    raw_base_url: &'static str,
    /// Source name for logging
    #[allow(dead_code)]
    source_name: &'static str,
}

/// Thumbnail matcher that caches results
pub struct ThumbnailMatcher {
    /// Loaded thumbnail indexes (priority order: boxarts, snaps)
    indexes: Arc<RwLock<Vec<LoadedIndex>>>,
    /// Cache of game_id -> matched thumbnail URL
    cache: Arc<RwLock<ThumbnailCache>>,
    /// Whether the indexes have been loaded
    loaded: Arc<RwLock<bool>>,
}

impl ThumbnailMatcher {
    pub fn new() -> Self {
        Self {
            indexes: Arc::new(RwLock::new(Vec::new())),
            cache: Arc::new(RwLock::new(HashMap::new())),
            loaded: Arc::new(RwLock::new(false)),
        }
    }

    /// Load all thumbnail indexes from libretro
    pub async fn load_index(&self) -> Result<usize> {
        info!("Fetching libretro thumbnail indexes...");

        let mut loaded_indexes = Vec::new();
        let mut total_count = 0;

        for source in THUMBNAIL_SOURCES {
            match Self::fetch_index(source).await {
                Ok(thumbnails) => {
                    let count = thumbnails.len();
                    info!("Loaded {} {} thumbnails", count, source.name);
                    total_count += count;
                    
                    loaded_indexes.push(LoadedIndex {
                        thumbnails,
                        raw_base_url: source.raw_base_url,
                        source_name: source.name,
                    });
                }
                Err(e) => {
                    warn!("Failed to load {} index: {}", source.name, e);
                    // Continue with other sources
                }
            }
        }

        if loaded_indexes.is_empty() {
            return Err(anyhow::anyhow!("Failed to load any thumbnail indexes"));
        }

        *self.indexes.write().await = loaded_indexes;
        *self.loaded.write().await = true;

        info!("Total: {} thumbnails loaded from {} sources", 
            total_count, 
            THUMBNAIL_SOURCES.len()
        );

        Ok(total_count)
    }

    /// Fetch a single thumbnail index
    async fn fetch_index(source: &ThumbnailSource) -> Result<Vec<String>> {
        let response = reqwest::get(source.index_url).await?;
        let html = response.text().await?;

        // Parse the HTML to extract .png filenames
        // The HTML contains links like: <a href="Game Name (Region).png">
        let mut thumbnails = Vec::new();
        
        for line in html.lines() {
            // Look for href="...png" patterns
            if let Some(start) = line.find("href=\"") {
                let rest = &line[start + 6..];
                if let Some(end) = rest.find("\"") {
                    let filename = &rest[..end];
                    if filename.ends_with(".png") && !filename.starts_with('?') {
                        // URL decode the filename and remove .png extension
                        let decoded = urlencoding_decode(filename);
                        let name = decoded.trim_end_matches(".png").to_string();
                        thumbnails.push(name);
                    }
                }
            }
        }

        Ok(thumbnails)
    }

    /// Check if the indexes are loaded
    pub async fn is_loaded(&self) -> bool {
        *self.loaded.read().await
    }

    /// Find the best matching thumbnail URL for a game title
    /// Searches boxarts first, then falls back to snaps
    /// Returns None if no good match is found (will use default PSP logo)
    pub async fn find_thumbnail(&self, game_id: &str, game_title: &str) -> Option<String> {
        // Check cache first
        {
            let cache = self.cache.read().await;
            if let Some(cached) = cache.get(game_id) {
                return cached.clone();
            }
        }

        // Ensure indexes are loaded
        if !self.is_loaded().await {
            warn!("Thumbnail indexes not loaded, attempting to load...");
            if let Err(e) = self.load_index().await {
                warn!("Failed to load thumbnail indexes: {}", e);
                return None;
            }
        }

        let indexes = self.indexes.read().await;
        
        // Normalize the game title for comparison
        let normalized_title = normalize_title(game_title);
        
        // Try each index in priority order (boxarts first, then snaps)
        let mut result: Option<String> = None;
        
        for index in indexes.iter() {
            if let Some(matched) = Self::find_best_match(&normalized_title, game_title, index) {
                result = Some(matched);
                break; // Found a match, stop searching
            }
        }

        // Cache the result (even if None, to avoid repeated lookups)
        {
            let mut cache = self.cache.write().await;
            cache.insert(game_id.to_string(), result.clone());
        }

        result
    }

    /// Find the best match in a single index
    fn find_best_match(normalized_title: &str, original_title: &str, index: &LoadedIndex) -> Option<String> {
        let mut best_match: Option<(f64, &str)> = None;
        
        for thumbnail_name in index.thumbnails.iter() {
            // Extract just the game name part (before region info in parentheses)
            let thumbnail_title = extract_game_name(thumbnail_name);
            let normalized_thumbnail = normalize_title(&thumbnail_title);
            
            let similarity = jaro_winkler(normalized_title, &normalized_thumbnail);
            
            if similarity >= MIN_SIMILARITY_THRESHOLD {
                if best_match.is_none() || similarity > best_match.unwrap().0 {
                    best_match = Some((similarity, thumbnail_name));
                }
            }
        }

        best_match.map(|(score, name)| {
            debug!(
                "Matched '{}' to '{}' with score {:.2}",
                original_title, name, score
            );
            // Construct the raw GitHub URL
            let encoded_name = urlencoding_encode(name);
            format!("{}{}.png", index.raw_base_url, encoded_name)
        })
    }

    /// Get cache statistics
    #[allow(dead_code)]
    pub async fn cache_stats(&self) -> (usize, usize) {
        let cache = self.cache.read().await;
        let hits = cache.values().filter(|v| v.is_some()).count();
        let misses = cache.values().filter(|v| v.is_none()).count();
        (hits, misses)
    }
}

impl Default for ThumbnailMatcher {
    fn default() -> Self {
        Self::new()
    }
}

/// Normalize a game title for comparison
fn normalize_title(title: &str) -> String {
    title
        .to_lowercase()
        // Remove common punctuation
        .replace([':', '-', '\'', '"', '!', '?', '.', ','], "")
        // Normalize spaces
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
}

/// Extract the game name from a thumbnail filename (before region info)
/// E.g., "God of War - Chains of Olympus (USA)" -> "God of War - Chains of Olympus"
fn extract_game_name(filename: &str) -> String {
    // Find the last opening parenthesis that likely contains region info
    if let Some(paren_pos) = filename.rfind(" (") {
        filename[..paren_pos].to_string()
    } else {
        filename.to_string()
    }
}

/// Simple URL decoding (handles common cases)
fn urlencoding_decode(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    
    while let Some(c) = chars.next() {
        if c == '%' {
            // Try to read two hex digits
            let hex: String = chars.by_ref().take(2).collect();
            if hex.len() == 2 {
                if let Ok(byte) = u8::from_str_radix(&hex, 16) {
                    result.push(byte as char);
                    continue;
                }
            }
            result.push('%');
            result.push_str(&hex);
        } else {
            result.push(c);
        }
    }
    
    result
}

/// URL encode a string for use in URLs
fn urlencoding_encode(s: &str) -> String {
    let mut result = String::with_capacity(s.len() * 3);
    
    for c in s.chars() {
        match c {
            'A'..='Z' | 'a'..='z' | '0'..='9' | '-' | '_' | '.' | '~' => {
                result.push(c);
            }
            ' ' => {
                result.push_str("%20");
            }
            _ => {
                for byte in c.to_string().as_bytes() {
                    result.push_str(&format!("%{:02X}", byte));
                }
            }
        }
    }
    
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_normalize_title() {
        assert_eq!(normalize_title("God of War: Chains of Olympus"), "god of war chains of olympus");
        assert_eq!(normalize_title("Monster Hunter Freedom Unite"), "monster hunter freedom unite");
    }

    #[test]
    fn test_extract_game_name() {
        assert_eq!(
            extract_game_name("God of War - Chains of Olympus (USA)"),
            "God of War - Chains of Olympus"
        );
        assert_eq!(
            extract_game_name("Monster Hunter Freedom Unite (Europe) (En,Fr,De,Es,It)"),
            "Monster Hunter Freedom Unite"
        );
    }

    #[test]
    fn test_urlencoding() {
        assert_eq!(urlencoding_decode("God%20of%20War"), "God of War");
        assert_eq!(urlencoding_encode("God of War"), "God%20of%20War");
    }
}
