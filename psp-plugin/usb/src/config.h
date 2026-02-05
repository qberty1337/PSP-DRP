/**
 * USB Plugin Configuration Module
 * Simplified config for USB mode - reads from same INI as net plugin
 */

#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#include <stdint.h>

/* Config file path on memory stick */
#define CONFIG_PATH "ms0:/seplugins/pspdrp/psp_drp.ini"

/* Maximum lengths */
#define MAX_NAME_LENGTH 32

/**
 * USB Plugin configuration structure
 */
typedef struct {
  /* Enable/disable plugin */
  int enabled;

  /* Custom PSP name for display */
  char psp_name[MAX_NAME_LENGTH];

  /* Game poll interval (milliseconds) */
  uint32_t poll_interval_ms;

  /* Heartbeat interval (milliseconds) */
  uint32_t heartbeat_interval_ms;

  /* Game info resend interval (milliseconds) - resend even if unchanged */
  uint32_t game_update_interval_ms;

  /* Enable logging to memory stick */
  int enable_logging;

  /* Send game icons when requested */
  int send_icons;

  /* Vblank wait count before USB init (default: 300 = ~5 seconds at 60fps) */
  uint32_t vblank_wait;

  /* Send once mode: send update then exit */
  int send_once;
} UsbPluginConfig;

/* Per-game vblank wait override */
int usb_config_get_game_vblank_wait(const char *game_id, uint32_t default_wait);

/**
 * Set default configuration values
 */
void usb_config_set_defaults(UsbPluginConfig *config);

/**
 * Load configuration from file
 * @return 0 on success, negative on error
 */
int usb_config_load(UsbPluginConfig *config);

#endif /* USB_CONFIG_H */
