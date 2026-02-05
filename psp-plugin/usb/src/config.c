/**
 * USB Plugin Configuration Implementation
 * Simplified config loader for USB mode
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <string.h>

#include "config.h"

/* INI parsing helpers */
static void trim_whitespace(char *str);
static int parse_bool(const char *value);
static void parse_line(const char *line, UsbPluginConfig *config);

/* Simple atoi replacement for kernel mode (no libc) */
static int parse_int(const char *str) {
  int result = 0;
  int sign = 1;

  /* Skip whitespace */
  while (*str == ' ' || *str == '\t')
    str++;

  /* Handle sign */
  if (*str == '-') {
    sign = -1;
    str++;
  } else if (*str == '+') {
    str++;
  }

  /* Parse digits */
  while (*str >= '0' && *str <= '9') {
    result = result * 10 + (*str - '0');
    str++;
  }

  return result * sign;
}

/**
 * Set default configuration values
 */
void usb_config_set_defaults(UsbPluginConfig *config) {
  memset(config, 0, sizeof(UsbPluginConfig));

  config->enabled = 1;
  strcpy(config->psp_name, "PSP");
  config->poll_interval_ms = 5000;
  config->heartbeat_interval_ms = 30000;
  config->game_update_interval_ms = 0; /* 0 = disabled */
  config->enable_logging = 0;
  config->send_icons = 1;
  config->vblank_wait = 300; /* ~5 seconds at 60fps */
  config->send_once = 0;
}

/**
 * Load configuration from file
 */
int usb_config_load(UsbPluginConfig *config) {
  SceUID fd;
  char buffer[2048];
  char line[128];
  int bytes_read;
  int i, j;

  /* Set defaults first */
  usb_config_set_defaults(config);

  /* Open config file */
  fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    /* Config doesn't exist, use defaults */
    return 0;
  }

  /* Read entire file */
  bytes_read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    return -1;
  }

  buffer[bytes_read] = '\0';

  /* Parse line by line */
  j = 0;
  for (i = 0; i <= bytes_read; i++) {
    if (buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == '\0') {
      line[j] = '\0';
      if (j > 0) {
        parse_line(line, config);
      }
      j = 0;

      /* Skip \r\n sequences */
      if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
        i++;
      }
    } else if (j < (int)sizeof(line) - 1) {
      line[j++] = buffer[i];
    }
  }

  return 0;
}

/**
 * Parse a single config line
 */
static void parse_line(const char *line, UsbPluginConfig *config) {
  char key[32];
  char value[64];
  const char *eq;
  int key_len, value_len;

  /* Skip comments and empty lines */
  if (line[0] == ';' || line[0] == '#' || line[0] == '\0') {
    return;
  }

  /* Find = sign */
  eq = strchr(line, '=');
  if (eq == NULL) {
    return;
  }

  /* Extract key */
  key_len = eq - line;
  if (key_len >= (int)sizeof(key)) {
    key_len = sizeof(key) - 1;
  }
  strncpy(key, line, key_len);
  key[key_len] = '\0';
  trim_whitespace(key);

  /* Extract value */
  value_len = strlen(eq + 1);
  if (value_len >= (int)sizeof(value)) {
    value_len = sizeof(value) - 1;
  }
  strncpy(value, eq + 1, value_len);
  value[value_len] = '\0';
  trim_whitespace(value);

  /* Parse known keys (only USB-relevant ones) */
  if (strcmp(key, "enabled") == 0) {
    config->enabled = parse_bool(value);
  } else if (strcmp(key, "psp_name") == 0) {
    strncpy(config->psp_name, value, sizeof(config->psp_name) - 1);
    config->psp_name[sizeof(config->psp_name) - 1] = '\0';
  } else if (strcmp(key, "poll_interval_ms") == 0) {
    config->poll_interval_ms = (uint32_t)parse_int(value);
    if (config->poll_interval_ms < 500) {
      config->poll_interval_ms = 500;
    }
    if (config->poll_interval_ms > 60000) {
      config->poll_interval_ms = 60000;
    }
  } else if (strcmp(key, "heartbeat_interval_ms") == 0) {
    config->heartbeat_interval_ms = (uint32_t)parse_int(value);
    if (config->heartbeat_interval_ms < 1000) {
      config->heartbeat_interval_ms = 1000;
    }
    if (config->heartbeat_interval_ms > 300000) {
      config->heartbeat_interval_ms = 300000;
    }
  } else if (strcmp(key, "game_update_interval_ms") == 0) {
    config->game_update_interval_ms = (uint32_t)parse_int(value);
    if (config->game_update_interval_ms > 300000) {
      config->game_update_interval_ms = 300000;
    }
  } else if (strcmp(key, "enable_logging") == 0) {
    config->enable_logging = parse_bool(value);
  } else if (strcmp(key, "send_icons") == 0) {
    config->send_icons = parse_bool(value);
  } else if (strcmp(key, "vblank_wait") == 0) {
    config->vblank_wait = (uint32_t)parse_int(value);
    if (config->vblank_wait > 3000) {
      config->vblank_wait = 3000; /* Max ~50 seconds */
    }
  } else if (strcmp(key, "send_once") == 0) {
    config->send_once = parse_bool(value);
  }
}

/**
 * Trim leading and trailing whitespace
 */
static void trim_whitespace(char *str) {
  char *start = str;
  char *end;

  /* Skip leading whitespace */
  while (*start == ' ' || *start == '\t') {
    start++;
  }

  /* Move to beginning if needed */
  if (start != str) {
    memmove(str, start, strlen(start) + 1);
  }

  /* Trim trailing whitespace */
  end = str + strlen(str) - 1;
  while (end > str && (*end == ' ' || *end == '\t')) {
    *end-- = '\0';
  }
}

/**
 * Parse a boolean value
 */
static int parse_bool(const char *value) {
  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
      strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
    return 1;
  }
  return 0;
}

/**
 * Get per-game vblank wait override
 * Reads GAMEID_vblank_wait from config file
 * Returns default_wait if not found
 */
int usb_config_get_game_vblank_wait(const char *game_id,
                                    uint32_t default_wait) {
  SceUID fd;
  char buffer[2048];
  char search_key[32];
  int bytes_read;
  int i, j;

  if (game_id == NULL || game_id[0] == '\0') {
    return (int)default_wait;
  }

  /* Build search key: GAMEID_vblank_wait */
  strncpy(search_key, game_id, sizeof(search_key) - 13);
  search_key[sizeof(search_key) - 13] = '\0';
  strcat(search_key, "_vblank_wait");

  /* Open config file */
  fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return (int)default_wait;
  }

  /* Read entire file */
  bytes_read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    return (int)default_wait;
  }

  buffer[bytes_read] = '\0';

  /* Simple line-by-line search */
  char line[128];
  j = 0;
  for (i = 0; i <= bytes_read; i++) {
    if (buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == '\0') {
      line[j] = '\0';
      if (j > 0) {
        /* Check if this line starts with our search key */
        char *eq = strchr(line, '=');
        if (eq != NULL) {
          int key_len = eq - line;
          /* Trim spaces from key */
          while (key_len > 0 &&
                 (line[key_len - 1] == ' ' || line[key_len - 1] == '\t')) {
            key_len--;
          }
          if (key_len == (int)strlen(search_key) &&
              strncmp(line, search_key, key_len) == 0) {
            /* Found it - parse value */
            const char *val = eq + 1;
            while (*val == ' ' || *val == '\t')
              val++;
            return parse_int(val);
          }
        }
      }
      j = 0;
      if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
        i++;
      }
    } else if (j < (int)sizeof(line) - 1) {
      line[j++] = buffer[i];
    }
  }

  return (int)default_wait;
}
