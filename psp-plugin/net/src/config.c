/**
 * Configuration Implementation
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "discord_rpc.h"

/* INI parsing helpers */
static void trim_whitespace(char *str);
static int parse_bool(const char *value);
static void parse_line(const char *line, PluginConfig *config);
static void copy_str(char *dst, size_t dst_size, const char *src);

/**
 * Set default configuration values
 */
void config_set_defaults(PluginConfig *config) {
  memset(config, 0, sizeof(PluginConfig));

  config->enabled = 1;
  config->desktop_ip[0] = '\0'; /* Empty = use discovery */
  config->port = DEFAULT_PORT;
  config->auto_discovery = 1;
  config->always_active = 0;
  config->send_icons = 1;
  strcpy(config->psp_name, "PSP");
  config->poll_interval_ms = 5000;
  config->heartbeat_interval_ms = 30000;
  config->game_update_interval_ms = 60000;
  config->connect_timeout_s = 30;
  config->send_once = 0;
  config->enable_logging = 0;
}

/**
 * Load configuration from file
 */
int config_load(PluginConfig *config) {
  SceUID fd;
  char buffer[2048];
  char line[128];
  int bytes_read;
  int i, j;

  /* Set defaults first */
  config_set_defaults(config);

  /* Open config file */
  fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    /* Config doesn't exist, create with defaults */
    config_save(config);
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
 * Save configuration to file
 */
int config_save(const PluginConfig *config) {
  SceUID fd;
  char buffer[2048];
  int len;

  fd = sceIoOpen(CONFIG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
  if (fd < 0) {
    return fd;
  }

  len = snprintf(
      buffer, sizeof(buffer),
      "; PSP Discord Rich Presence Configuration\n"
      "; Edit this file to customize the plugin behavior\n"
      "\n"
      "; Enable or disable the plugin (1 = enabled, 0 = disabled)\n"
      "enabled = %d\n"
      "\n"
      "; Desktop companion app IP address\n"
      "; Leave empty to use auto-discovery\n"
      "desktop_ip = %s\n"
      "\n"
      "; Port to connect to (default: 9276)\n"
      "port = %d\n"
      "\n"
      "; Enable auto-discovery of desktop app (1 = enabled, 0 = disabled)\n"
      "auto_discovery = %d\n"
      "\n"
      "; When to show presence:\n"
      "; 0 = only when playing games\n"
      "; 1 = always (including XMB, videos, music)\n"
      "always_active = %d\n"
      "\n"
      "; Send game icons to desktop app (1 = enabled, 0 = disabled)\n"
      "send_icons = %d\n"
      "\n"
      "; Custom name for this PSP (shown in Discord)\n"
      "psp_name = %s\n"
      "\n"
      "; Game polling interval in milliseconds (default: 5000)\n"
      "poll_interval_ms = %lu\n"
      "\n"
      "; Heartbeat interval in milliseconds (default: 30000)\n"
      "heartbeat_interval_ms = %lu\n"
      "\n"
      "; Game info resend interval in milliseconds (default: 60000)\n"
      "; Set to 0 to only send on change\n"
      "game_update_interval_ms = %lu\n"
      "\n"
      "; Connect timeout in seconds (0 = disable, default: 30)\n"
      "connect_timeout_s = %lu\n"
      "\n"
      "; Send once mode (1 = enabled, 0 = disabled)\n"
      "; When enabled, sends one update on plugin load then unloads network\n"
      "send_once = %d\n",
      config->enabled, config->desktop_ip, config->port, config->auto_discovery,
      config->always_active, config->send_icons, config->psp_name,
      (unsigned long)config->poll_interval_ms,
      (unsigned long)config->heartbeat_interval_ms,
      (unsigned long)config->game_update_interval_ms,
      (unsigned long)config->connect_timeout_s, config->send_once);

  sceIoWrite(fd, buffer, len);
  sceIoClose(fd);

  return 0;
}

/**
 * Parse a single config line
 */
static void parse_line(const char *line, PluginConfig *config) {
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

  /* Parse known keys */
  if (strcmp(key, "enabled") == 0) {
    config->enabled = parse_bool(value);
  } else if (strcmp(key, "desktop_ip") == 0) {
    copy_str(config->desktop_ip, sizeof(config->desktop_ip), value);
  } else if (strcmp(key, "port") == 0) {
    config->port = atoi(value);
    if (config->port == 0) {
      config->port = DEFAULT_PORT;
    }
  } else if (strcmp(key, "auto_discovery") == 0) {
    config->auto_discovery = parse_bool(value);
  } else if (strcmp(key, "always_active") == 0) {
    config->always_active = parse_bool(value);
  } else if (strcmp(key, "send_icons") == 0) {
    config->send_icons = parse_bool(value);
  } else if (strcmp(key, "psp_name") == 0) {
    copy_str(config->psp_name, sizeof(config->psp_name), value);
  } else if (strcmp(key, "poll_interval_ms") == 0) {
    config->poll_interval_ms = (uint32_t)atoi(value);
    if (config->poll_interval_ms < 500) {
      config->poll_interval_ms = 500;
    }
    if (config->poll_interval_ms > 60000) {
      config->poll_interval_ms = 60000;
    }
  } else if (strcmp(key, "heartbeat_interval_ms") == 0) {
    config->heartbeat_interval_ms = (uint32_t)atoi(value);
    if (config->heartbeat_interval_ms < 1000) {
      config->heartbeat_interval_ms = 1000;
    }
    if (config->heartbeat_interval_ms > 300000) {
      config->heartbeat_interval_ms = 300000;
    }
  } else if (strcmp(key, "game_update_interval_ms") == 0) {
    config->game_update_interval_ms = (uint32_t)atoi(value);
    if (config->game_update_interval_ms > 3600000) {
      config->game_update_interval_ms = 3600000;
    }
  } else if (strcmp(key, "connect_timeout_s") == 0) {
    uint32_t t = (uint32_t)atoi(value);
    if (t > 600) {
      t = 600;
    }
    config->connect_timeout_s = t;
  } else if (strcmp(key, "send_once") == 0) {
    config->send_once = parse_bool(value);
  } else if (strcmp(key, "enable_logging") == 0) {
    config->enable_logging = parse_bool(value);
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
 * Safe string copy with null termination
 */
static void copy_str(char *dst, size_t dst_size, const char *src) {
  size_t len;
  if (dst == NULL || dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
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
 * Get game-specific startup delay from config
 * Looks for GAMEID_startup_delay_ms = VALUE
 * Returns -1 if not found or empty value
 */
int config_get_game_startup_delay(const char *game_id) {
  SceUID fd;
  char buffer[4096];
  char target_key[48];
  int bytes_read;
  char *found;
  char *eq;
  char *val_start;
  char *line_start;
  int delay;

  if (game_id == NULL || game_id[0] == '\0') {
    return -1;
  }

  /* Build the key we're looking for: GAMEID_startup_delay_ms */
  snprintf(target_key, sizeof(target_key), "%s_startup_delay_ms", game_id);

  fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  bytes_read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    return -1;
  }

  buffer[bytes_read] = '\0';

  /* Search for the key in the buffer */
  found = strstr(buffer, target_key);
  while (found != NULL) {
    /* Check if this occurrence is at the start of a line (not a comment) */
    line_start = found;

    /* Walk backwards to find line start */
    while (line_start > buffer && *(line_start - 1) != '\n' &&
           *(line_start - 1) != '\r') {
      line_start--;
    }

    /* Skip whitespace at line start */
    while (*line_start == ' ' || *line_start == '\t') {
      line_start++;
    }

    /* If line starts with ; or #, it's a comment - skip */
    if (*line_start == ';' || *line_start == '#') {
      found = strstr(found + 1, target_key);
      continue;
    }

    /* Make sure the key starts at the beginning of the non-comment part */
    if (line_start != found) {
      found = strstr(found + 1, target_key);
      continue;
    }

    /* Find the = after the key */
    eq = strchr(found, '=');
    if (eq == NULL) {
      found = strstr(found + 1, target_key);
      continue;
    }

    /* Get the value after = */
    val_start = eq + 1;
    while (*val_start == ' ' || *val_start == '\t') {
      val_start++;
    }

    /* Check for empty value */
    if (*val_start == '\0' || *val_start == '\n' || *val_start == '\r' ||
        *val_start == ';') {
      return -1;
    }

    /* Parse the delay value */
    delay = atoi(val_start);
    if (delay >= 0) {
      return delay;
    }
    return -1;
  }

  return -1;
}

/**
 * Check if a game delay key already exists in config
 */
static int config_has_game_delay_key(const char *game_id) {
  SceUID fd;
  char buffer[2048];
  char target_key[32];
  int bytes_read;
  char *found;

  if (game_id == NULL || game_id[0] == '\0') {
    return 0;
  }

  snprintf(target_key, sizeof(target_key), "%s_startup_delay_ms", game_id);

  fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return 0;
  }

  bytes_read = sceIoRead(fd, buffer, sizeof(buffer) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    return 0;
  }

  buffer[bytes_read] = '\0';
  found = strstr(buffer, target_key);
  return (found != NULL) ? 1 : 0;
}

/**
 * Write a placeholder for game-specific startup delay
 * Appends "GAMEID_startup_delay_ms =" to config for user to fill in
 */
int config_write_game_delay_placeholder(const char *game_id) {
  SceUID fd;
  char line[128];
  int len;

  if (game_id == NULL || game_id[0] == '\0') {
    return -1;
  }

  /* Don't write if it already exists */
  if (config_has_game_delay_key(game_id)) {
    return 0;
  }

  fd = sceIoOpen(CONFIG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) {
    return fd;
  }

  /* Write the placeholder with a comment */
  len = snprintf(line, sizeof(line),
                 "\n; Game-specific startup delay (network init failed)\n"
                 "%s_startup_delay_ms = \n",
                 game_id);

  /* Ensure we don't write more than the buffer */
  if (len > (int)sizeof(line) - 1) {
    len = (int)sizeof(line) - 1;
  }

  sceIoWrite(fd, line, len);
  sceIoClose(fd);

  return 0;
}
