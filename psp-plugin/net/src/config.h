/**
 * Configuration Module
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Config file path on memory stick */
#define CONFIG_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp.ini"

/* Maximum lengths */
#define MAX_IP_LENGTH 16
#define MAX_NAME_LENGTH 32

/**
 * Plugin configuration structure
 */
typedef struct {
  /* Enable/disable plugin */
  int enabled;

  /* Desktop companion IP address */
  char desktop_ip[MAX_IP_LENGTH];

  /* Port to connect to */
  uint16_t port;

  /* Enable auto-discovery */
  int auto_discovery;

  /* When to be active: 0=game only, 1=always */
  int always_active;

  /* Send game icons */
  int send_icons;

  /* Custom PSP name for display */
  char psp_name[MAX_NAME_LENGTH];

  /* Game poll interval (milliseconds) */
  uint32_t poll_interval_ms;

  /* Heartbeat interval (milliseconds) */
  uint32_t heartbeat_interval_ms;

  /* Game info resend interval (milliseconds) */
  uint32_t game_update_interval_ms;

  /* Connect timeout (seconds, 0 = disable) */
  uint32_t connect_timeout_s;
} PluginConfig;

/**
 * Set default configuration values
 *
 * @param config Configuration to initialize
 */
void config_set_defaults(PluginConfig *config);

/**
 * Load configuration from file
 *
 * @param config Output configuration
 * @return 0 on success, negative on error
 */
int config_load(PluginConfig *config);

/**
 * Save configuration to file
 *
 * @param config Configuration to save
 * @return 0 on success, negative on error
 */
int config_save(const PluginConfig *config);

#endif /* CONFIG_H */
