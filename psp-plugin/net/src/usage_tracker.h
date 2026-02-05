/**
 * Local Usage Tracker
 *
 * Tracks game play sessions and writes usage.json for the PSP Stats App.
 * Only active when offline_mode=1 in the config.
 */

#ifndef USAGE_TRACKER_H
#define USAGE_TRACKER_H

#include <stdint.h>

/* Maximum game entries to track */
#define MAX_TRACKED_GAMES 50

/* Maximum game title length */
#define MAX_GAME_TITLE 128

/* Maximum daily playtime entries per game (about 1 month of history) */
#define MAX_DAILY_ENTRIES 30

/* Path to usage_log.json on memory stick (matches Desktop Companion) */
#define USAGE_JSON_PATH "ms0:/SEPLUGINS/pspdrp/usage_log.json"

/**
 * Per-day playtime entry
 */
typedef struct {
  char date[12];    /* Date in YYYY-MM-DD format */
  uint32_t seconds; /* Playtime in seconds for this day */
} DailyPlaytime;

/**
 * Single game's usage data
 */
typedef struct {
  char game_id[16];                       /* Game ID (e.g., "NPUH10117") */
  char title[MAX_GAME_TITLE];             /* Game title */
  uint64_t total_seconds;                 /* Total play time in seconds */
  uint32_t session_count;                 /* Number of play sessions */
  uint32_t daily_count;                   /* Number of daily entries */
  DailyPlaytime daily[MAX_DAILY_ENTRIES]; /* Per-day playtime */
} GameUsage;

/**
 * Overall usage data structure
 */
typedef struct {
  uint32_t total_games;    /* Number of unique games played */
  uint64_t total_playtime; /* Total playtime across all games */
  uint64_t last_updated;   /* Unix timestamp of last modification */
  GameUsage games[MAX_TRACKED_GAMES];
} UsageData;

/**
 * Initialize the usage tracker
 * Loads existing usage.json if present
 */
void usage_init(void);

/**
 * Start a new game session
 *
 * @param game_id Game ID string
 * @param title Game title string
 */
void usage_start_session(const char *game_id, const char *title);

/**
 * End the current game session
 * Calculates elapsed time and updates usage data
 */
void usage_end_session(void);

/**
 * Force save usage data to disk
 * Called periodically and on shutdown
 */
void usage_save(void);

/**
 * Set PSP name for JSON output
 * @param name PSP name to use in the JSON file
 */
void usage_set_psp_name(const char *name);

/**
 * Check if there's an active session
 * @return 1 if session active, 0 otherwise
 */
int usage_has_active_session(void);

/**
 * Get the last_updated timestamp
 * @return Unix timestamp of last modification
 */
uint64_t usage_get_last_updated(void);

/**
 * Get pointer to the in-memory usage data
 * Used for serialization during sync
 * @return Pointer to UsageData (read-only)
 */
const UsageData *usage_get_data(void);

/**
 * Merge remote usage data into local
 * Uses high-water-mark strategy (higher values win)
 * @param json_data JSON string containing remote usage data
 * @param len Length of JSON string
 * @return 0 on success, -1 on error
 */
int usage_merge_remote(const char *json_data, size_t len);

/**
 * Serialize usage data to JSON string
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written, or -1 on error
 */
int usage_serialize_json(char *buffer, size_t buffer_size);

#endif /* USAGE_TRACKER_H */
