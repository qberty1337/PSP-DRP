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

/* Helper: get current tick count */
static uint64_t get_tick(void) {
  uint64_t tick;
  sceRtcGetCurrentTick(&tick);
  return tick;
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
  g_tick_resolution = sceRtcGetTickResolution();

  if (!g_data_loaded) {
    load_usage_json();
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
 */
void usage_save(void) {
  SceUID fd;
  char buffer[4096];
  int len, i;
  int first_game = 1;

  /* End current session to capture time */
  if (g_current_session.active) {
    /* Save session data but don't clear it */
    uint64_t elapsed_ticks = get_tick() - g_current_session.start_tick;
    uint64_t elapsed_seconds = elapsed_ticks / g_tick_resolution;

    if (elapsed_seconds >= 1) {
      GameUsage *game = find_or_create_game(g_current_session.game_id);
      if (game) {
        if (g_current_session.title[0] != '\0') {
          copy_str(game->title, sizeof(game->title), g_current_session.title);
        }
        /* Don't add to totals yet - will be added when session ends */
      }
    }
  }

  /* Build JSON */
  len = snprintf(buffer, sizeof(buffer),
                 "{\"total_games\":%lu,\"total_playtime\":%llu,\"games\":[",
                 (unsigned long)g_usage_data.total_games,
                 (unsigned long long)g_usage_data.total_playtime);

  for (i = 0;
       i < (int)g_usage_data.total_games && len < (int)sizeof(buffer) - 200;
       i++) {
    GameUsage *game = &g_usage_data.games[i];
    len += snprintf(buffer + len, sizeof(buffer) - len,
                    "%s{\"title\":\"%s\",\"game_id\":\"%s\",\"seconds\":%llu,"
                    "\"sessions\":%lu}",
                    first_game ? "" : ",", game->title, game->game_id,
                    (unsigned long long)game->total_seconds,
                    (unsigned long)game->session_count);
    first_game = 0;
  }

  len += snprintf(buffer + len, sizeof(buffer) - len, "]}");

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
