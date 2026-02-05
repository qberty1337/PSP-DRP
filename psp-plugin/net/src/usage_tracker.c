/**
 * Local Usage Tracker Implementation
 *
 * Tracks game play sessions and writes usage.json for the PSP Stats App.
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usage_tracker.h"

/* External logging function from main.c */
extern void net_log(const char *fmt, ...);

/* Current session state */
static struct {
  int active;
  char game_id[16];
  char title[MAX_GAME_TITLE];
  uint64_t start_tick;
} g_current_session;

/* Loaded usage data */
static UsageData g_usage_data;
static int g_data_loaded = 0;

/* Tick resolution (ticks per second) */
static uint32_t g_tick_resolution = 1000000;

/* PSP name for JSON output (set via usage_set_psp_name) */
static char g_psp_name[32] = "PSP";

/* Helper: safe string copy */
static void copy_str(char *dst, size_t dst_size, const char *src) {
  size_t len;
  if (dst == NULL || dst_size == 0)
    return;
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  len = strlen(src);
  if (len >= dst_size)
    len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/* Helper: get current tick count (microseconds) */
static uint64_t get_tick(void) {
  SceKernelSysClock clock;
  sceKernelGetSystemTime(&clock);
  return clock.low + ((uint64_t)clock.hi << 32);
}

/* Helper: get current time as formatted string "YYYY-MM-DD HH:MM:SS" */
static void get_current_time_string(char *buffer, size_t size) {
  ScePspDateTime rtc_time;
  if (sceRtcGetCurrentClockLocalTime(&rtc_time) >= 0) {
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d", rtc_time.year,
             rtc_time.month, rtc_time.day, rtc_time.hour, rtc_time.minute,
             rtc_time.second);
  } else {
    buffer[0] = '\0';
  }
}

/* Helper: find or create game entry */
static GameUsage *find_or_create_game(const char *game_id) {
  uint32_t i;

  /* Search for existing entry */
  for (i = 0; i < g_usage_data.total_games; i++) {
    if (strcmp(g_usage_data.games[i].game_id, game_id) == 0) {
      return &g_usage_data.games[i];
    }
  }

  /* Create new entry if space available */
  if (g_usage_data.total_games < MAX_TRACKED_GAMES) {
    GameUsage *game = &g_usage_data.games[g_usage_data.total_games];
    memset(game, 0, sizeof(GameUsage));
    copy_str(game->game_id, sizeof(game->game_id), game_id);
    g_usage_data.total_games++;
    return game;
  }

  return NULL; /* Full */
}

/* Parse a simple JSON number after a key */
static uint64_t parse_json_number(const char *json, const char *key) {
  char search[64];
  const char *pos;
  snprintf(search, sizeof(search), "\"%s\":", key);
  pos = strstr(json, search);
  if (pos) {
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t')
      pos++;
    return (uint64_t)atoll(pos);
  }
  return 0;
}

/* Parse a simple JSON string value */
static void parse_json_string(const char *json, const char *key, char *out,
                              size_t out_size) {
  char search[64];
  const char *pos, *end;
  size_t len;

  out[0] = '\0';
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  pos = strstr(json, search);
  if (pos) {
    pos += strlen(search);
    end = strchr(pos, '"');
    if (end) {
      len = end - pos;
      if (len >= out_size)
        len = out_size - 1;
      memcpy(out, pos, len);
      out[len] = '\0';
    }
  }
}

/* Load usage data from JSON file */
static void load_usage_json(void) {
  SceUID fd;
  char buffer[4096];
  int bytes_read;
  const char *games_start, *game_start, *game_end;

  memset(&g_usage_data, 0, sizeof(g_usage_data));

  fd = sceIoOpen(USAGE_JSON_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    g_data_loaded = 1;
    return;
  }

  bytes_read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    g_data_loaded = 1;
    return;
  }
  buffer[bytes_read] = '\0';

  /* Parse top-level fields */
  g_usage_data.total_playtime = parse_json_number(buffer, "total_playtime");

  /* Find games array */
  games_start = strstr(buffer, "\"games\":");
  if (games_start) {
    games_start = strchr(games_start, '[');
    if (games_start) {
      games_start++;

      /* Parse each game object */
      game_start = strchr(games_start, '{');
      while (game_start && g_usage_data.total_games < MAX_TRACKED_GAMES) {
        game_end = strchr(game_start, '}');
        if (!game_end)
          break;

        /* Extract a single game */
        {
          char game_json[512];
          size_t len = game_end - game_start + 1;
          if (len >= sizeof(game_json))
            len = sizeof(game_json) - 1;
          memcpy(game_json, game_start, len);
          game_json[len] = '\0';

          GameUsage *game = &g_usage_data.games[g_usage_data.total_games];
          memset(game, 0, sizeof(GameUsage));

          parse_json_string(game_json, "game_id", game->game_id,
                            sizeof(game->game_id));
          if (game->game_id[0] == '\0') {
            /* Try alternate key name */
            parse_json_string(game_json, "id", game->game_id,
                              sizeof(game->game_id));
          }
          parse_json_string(game_json, "title", game->title,
                            sizeof(game->title));

          /* Try both field name variants */
          game->total_seconds = parse_json_number(game_json, "seconds");
          if (game->total_seconds == 0) {
            game->total_seconds = parse_json_number(game_json, "total_seconds");
          }
          game->session_count =
              (uint32_t)parse_json_number(game_json, "sessions");
          if (game->session_count == 0) {
            game->session_count =
                (uint32_t)parse_json_number(game_json, "session_count");
          }

          if (game->game_id[0] != '\0') {
            g_usage_data.total_games++;
          }
        }

        game_start = strchr(game_end, '{');
      }
    }
  }

  g_data_loaded = 1;
}

/**
 * Initialize the usage tracker
 */
void usage_init(void) {
  memset(&g_current_session, 0, sizeof(g_current_session));
  /* Tick resolution is 1000000 (microseconds) since we use
   * sceKernelGetSystemTime */
  g_tick_resolution = 1000000;

  if (!g_data_loaded) {
    load_usage_json();
  }
}

/**
 * Set PSP name for JSON output
 */
void usage_set_psp_name(const char *name) {
  if (name && name[0] != '\0') {
    copy_str(g_psp_name, sizeof(g_psp_name), name);
  }
}

/**
 * Start a new game session
 */
void usage_start_session(const char *game_id, const char *title) {
  /* End any existing session first */
  if (g_current_session.active) {
    usage_end_session();
  }

  if (game_id == NULL || game_id[0] == '\0') {
    return;
  }

  g_current_session.active = 1;
  copy_str(g_current_session.game_id, sizeof(g_current_session.game_id),
           game_id);
  copy_str(g_current_session.title, sizeof(g_current_session.title),
           title ? title : game_id);
  g_current_session.start_tick = get_tick();
}

/**
 * End the current game session
 */
void usage_end_session(void) {
  uint64_t elapsed_ticks, elapsed_seconds;
  GameUsage *game;

  if (!g_current_session.active) {
    return;
  }

  /* Calculate elapsed time */
  elapsed_ticks = get_tick() - g_current_session.start_tick;
  elapsed_seconds = elapsed_ticks / g_tick_resolution;

  /* Only count sessions >= 1 second */
  if (elapsed_seconds >= 1) {
    game = find_or_create_game(g_current_session.game_id);
    if (game) {
      /* Update title if different */
      if (g_current_session.title[0] != '\0') {
        copy_str(game->title, sizeof(game->title), g_current_session.title);
      }
      game->total_seconds += elapsed_seconds;
      game->session_count++;
      g_usage_data.total_playtime += elapsed_seconds;
    }
  }

  g_current_session.active = 0;
  g_current_session.game_id[0] = '\0';
  g_current_session.title[0] = '\0';
}

/**
 * Save usage data to JSON file
 * Format matches desktop companion:
 * {"psps":{"PSP Name":{"psp_name":"PSP Name","games":{"GAMEID:1":{...}}}}}
 */
void usage_save(void) {
  SceUID fd;
  static char
      buffer[4096]; /* Static buffer - need more space for nested format */
  int len, i;
  int first_game = 1;
  uint64_t elapsed_ticks;
  uint64_t elapsed_seconds;
  uint64_t current_session_seconds;
  GameUsage *game;
  const char *current_game_id;
  uint64_t game_seconds;
  uint32_t game_sessions;
  char last_played_str[24]; /* "YYYY-MM-DD HH:MM:SS" */

  /* Get current time for last_played */
  get_current_time_string(last_played_str, sizeof(last_played_str));

  /* Calculate current session time (if active) */
  current_session_seconds = 0;
  current_game_id = NULL;
  if (g_current_session.active) {
    elapsed_ticks = get_tick() - g_current_session.start_tick;
    elapsed_seconds = elapsed_ticks / g_tick_resolution;
    if (elapsed_seconds >= 1) {
      current_session_seconds = elapsed_seconds;
      current_game_id = g_current_session.game_id;

      /* Make sure the game entry exists with title */
      game = find_or_create_game(g_current_session.game_id);
      if (game && g_current_session.title[0] != '\0') {
        copy_str(game->title, sizeof(game->title), g_current_session.title);
      }
    }
  }

  /* Build JSON in desktop companion format */
  len = snprintf(buffer, sizeof(buffer),
                 "{\"psps\":{\"%s\":{\"psp_name\":\"%s\",\"games\":{",
                 g_psp_name, g_psp_name);

  for (i = 0;
       i < (int)g_usage_data.total_games && len < (int)sizeof(buffer) - 300;
       i++) {
    game = &g_usage_data.games[i];
    game_seconds = game->total_seconds;
    game_sessions = game->session_count;

    /* Add current session time to the matching game */
    if (current_game_id && strcmp(game->game_id, current_game_id) == 0) {
      game_seconds += current_session_seconds;
      game_sessions += 1; /* Count current session */
    }

    /* Key is "game_id:session_count" like desktop */
    len += snprintf(buffer + len, sizeof(buffer) - len,
                    "%s\"%s:%lu\":{\"game_id\":\"%s\",\"title\":\"%s\","
                    "\"total_seconds\":%llu,\"first_played\":\"\","
                    "\"last_played\":\"%s\",\"session_count\":%lu,"
                    "\"play_dates\":[],\"daily_playtime\":{}}",
                    first_game ? "" : ",", game->game_id,
                    (unsigned long)game_sessions, game->game_id, game->title,
                    (unsigned long long)game_seconds, last_played_str,
                    (unsigned long)game_sessions);
    first_game = 0;
  }

  len += snprintf(buffer + len, sizeof(buffer) - len,
                  "}}},\"last_updated\":null}");

  /* Small delay before file I/O to let XMB settle */
  sceKernelDelayThread(10 * 1000); /* 10ms */

  /* Write to file */
  fd = sceIoOpen(USAGE_JSON_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                 0777);
  if (fd >= 0) {
    sceIoWrite(fd, buffer, len);
    sceIoClose(fd);
  }
}

/**
 * Check if there's an active session
 */
int usage_has_active_session(void) { return g_current_session.active; }

/**
 * Get the last_updated timestamp
 */
uint64_t usage_get_last_updated(void) { return g_usage_data.last_updated; }

/**
 * Get pointer to the in-memory usage data
 */
const UsageData *usage_get_data(void) { return &g_usage_data; }

/**
 * Merge remote usage data into local (stub - not needed for basic offline)
 */
int usage_merge_remote(const char *json_data, size_t len) {
  (void)json_data;
  (void)len;
  return 0;
}

/**
 * Serialize usage data to JSON string
 */
int usage_serialize_json(char *buffer, size_t buffer_size) {
  int len, i;
  int first_game = 1;
  GameUsage *game;

  len = snprintf(buffer, buffer_size,
                 "{\"total_games\":%lu,\"total_playtime\":%llu,\"games\":[",
                 (unsigned long)g_usage_data.total_games,
                 (unsigned long long)g_usage_data.total_playtime);

  for (i = 0; i < (int)g_usage_data.total_games && len < (int)buffer_size - 200;
       i++) {
    game = &g_usage_data.games[i];
    len += snprintf(buffer + len, buffer_size - len,
                    "%s{\"title\":\"%s\",\"game_id\":\"%s\",\"seconds\":%llu,"
                    "\"sessions\":%lu}",
                    first_game ? "" : ",", game->title, game->game_id,
                    (unsigned long long)game->total_seconds,
                    (unsigned long)game->session_count);
    first_game = 0;
  }

  len += snprintf(buffer + len, buffer_size - len, "]}");
  return len;
}
