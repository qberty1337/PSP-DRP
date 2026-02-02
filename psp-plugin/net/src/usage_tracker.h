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

/* Path to usage.json on memory stick */
#define USAGE_JSON_PATH "ms0:/seplugins/pspdrp/usage.json"

/**
 * Single game's usage data
 */
typedef struct {
  char game_id[16];           /* Game ID (e.g., "NPUH10117") */
  char title[MAX_GAME_TITLE]; /* Game title */
  uint64_t total_seconds;     /* Total play time in seconds */
  uint32_t session_count;     /* Number of play sessions */
} GameUsage;

/**
 * Overall usage data structure
 */
typedef struct {
  uint32_t total_games;    /* Number of unique games played */
  uint64_t total_playtime; /* Total playtime across all games */
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
 * Check if there's an active session
 * @return 1 if session active, 0 otherwise
 */
int usage_has_active_session(void);

#endif /* USAGE_TRACKER_H */
