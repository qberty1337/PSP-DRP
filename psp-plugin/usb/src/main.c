/*
 * PSP DRP USB Kernel Module
 * Self-contained USB mode - handles game detection and data sending
 */

#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <string.h>

/* NOTE: usage_tracker requires stdlib functions not available in kernel mode
 * Stats sync is disabled in USB mode for now */
/* #include "../../net/src/usage_tracker.h" */
#include "config.h"
#include "game_detect.h"
#include "usb_driver.h"
#include "usb_protocol.h"

PSP_MODULE_INFO("PSPDRP_USB", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

/*============================================================================
 * Configuration and State
 *============================================================================*/

/* Startup args magic - indicates loader passed game info */
#define USB_STARTUP_MAGIC 0x55534247 /* "USBG" */

/* Startup args structure from loader */
typedef struct {
  uint32_t magic;
  char game_id[16];
  char game_title[64];
} UsbStartupArgs;

/* Thread state */
static volatile int g_running = 0;
static SceUID g_thread_id = -1;
static char g_last_game_id[16] = {0};
static char g_startup_game_id[16] = {0};    /* Game ID from loader */
static char g_startup_game_title[64] = {0}; /* Game title from loader */
static uint32_t g_game_start_time = 0;
static uint32_t g_last_heartbeat = 0;
static uint32_t g_last_game_update = 0; /* For game_update_interval_ms */

/* Configuration */
static UsbPluginConfig g_config;
int g_logging_enabled =
    0; /* Controls USB_LOG output (extern in usb_driver.h) */

/* Stats sync state (sync protocol only - no local usage tracking in USB mode)
 */
static int g_stats_sync_pending = 0;   /* 1 = waiting for response */
static int g_stats_sync_done = 0;      /* 1 = initial sync complete */
static uint32_t g_last_stats_sync = 0; /* Last sync attempt time (seconds) */
#define STATS_SYNC_INTERVAL_S (5 * 60) /* 5 minutes */

/*============================================================================
 * Helpers
 *============================================================================*/

/* Path to usage_log.json on memory stick (matches Desktop Companion) */
#define USAGE_JSON_PATH "ms0:/SEPLUGINS/pspdrp/usage_log.json"

/**
 * Read local last_updated timestamp from usage.json (kernel-safe)
 * Uses only PSP kernel I/O - no stdlib required
 * @return timestamp or 0 if file doesn't exist or can't be parsed
 */
static uint64_t read_local_timestamp(void) {
  SceUID fd;
  char buf[512]; /* Small buffer - we only need the last_updated field */
  int bytes_read;
  char *pos;
  uint64_t timestamp = 0;

  fd = sceIoOpen(USAGE_JSON_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return 0; /* File doesn't exist */
  }

  bytes_read = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);

  if (bytes_read <= 0) {
    return 0;
  }
  buf[bytes_read] = '\0';

  /* Simple search for "last_updated": - no stdlib string functions needed */
  pos = buf;
  while (*pos) {
    if (pos[0] == 'l' && pos[1] == 'a' && pos[2] == 's' && pos[3] == 't' &&
        pos[4] == '_' && pos[5] == 'u' && pos[6] == 'p' && pos[7] == 'd' &&
        pos[8] == 'a' && pos[9] == 't' && pos[10] == 'e' && pos[11] == 'd') {
      /* Found "last_updated" - skip to the number */
      pos += 12;
      while (*pos && (*pos == '"' || *pos == ':' || *pos == ' ')) {
        pos++;
      }
      /* Parse the number manually */
      while (*pos >= '0' && *pos <= '9') {
        timestamp = timestamp * 10 + (*pos - '0');
        pos++;
      }
      break;
    }
    pos++;
  }

  return timestamp;
}

/**
 * Write JSON data to usage.json (kernel-safe)
 * Uses only PSP kernel I/O - no stdlib required
 * @return 0 on success, negative on error
 */
static int write_usage_json(const char *json_data, size_t json_len) {
  SceUID fd;
  int ret;

  /* Truncate and write the new file */
  fd = sceIoOpen(USAGE_JSON_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                 0777);
  if (fd < 0) {
    USB_LOG_ERR("Failed to open usage.json for writing", fd);
    return fd;
  }

  ret = sceIoWrite(fd, json_data, json_len);
  sceIoClose(fd);

  if (ret < 0) {
    USB_LOG_ERR("Failed to write usage.json", ret);
    return ret;
  }

  USB_LOG("Saved stats to usage.json");
  return 0;
}

/* Wait for N vblanks - each vblank is ~16.67ms at 60fps.
 * NO LOGGING during wait - some games are sensitive to memory stick I/O. */
static void wait_for_vblanks(int count) {
  int i;
  for (i = 0; i < count && g_running; i++) {
    sceDisplayWaitVblankStart();
  }
}

/*============================================================================
 * Main Thread - Game Detection and USB Sending
 *============================================================================*/

static int usb_main_thread(SceSize args, void *argp) {
  GameInfo game;
  uint32_t last_poll = 0;
  uint32_t now;
  int connected_logged = 0;
  int loop_count = 0;

  (void)args;
  (void)argp;

  USB_LOG("USB thread started");

  /* Initialize game detection */
  game_detect_init();
  USB_LOG("Game detection initialized");

  /* Usage tracker not available in kernel mode
  usage_init();
  USB_LOG("Usage tracker initialized");
  */

  /* If loader passed a game ID, send it immediately */
  if (g_startup_game_id[0] != '\0') {
    USB_LOG("Sending game ID from loader immediately");

    /* Wait for USB connection first */
    int connect_wait = 0;
    while (!usb_driver_is_connected() && connect_wait < 50) {
      sceKernelDelayThread(100 * 1000); /* 100ms */
      connect_wait++;
    }

    if (usb_driver_is_connected()) {
      USB_LOG("Host connected, waiting for stabilization...");

      /* Wait for connection to stabilize - desktop needs time to re-establish
       */
      sceKernelDelayThread(2000 * 1000); /* 2 seconds */

      USB_LOG("Sending startup game info");
      strncpy(g_last_game_id, g_startup_game_id, sizeof(g_last_game_id) - 1);
      g_game_start_time = sceKernelGetSystemTimeLow() / 1000000;

      /* Use actual title if available, otherwise fall back to game ID */
      const char *title = (g_startup_game_title[0] != '\0')
                              ? g_startup_game_title
                              : g_startup_game_id;

      int ret = usb_send_game_info(g_startup_game_id, title, 1,
                                   1, /* state = 1 (playing), has_icon = 1 */
                                   g_game_start_time, 0, g_config.psp_name);

      if (ret >= 0) {
        USB_LOG("Sent startup game info successfully");
        g_last_game_update = sceKernelGetSystemTimeLow() / 1000000;
      } else {
        USB_LOG_ERR("Failed to send startup game info", ret);
      }
    } else {
      USB_LOG("USB not connected, can't send startup game");
    }
  } else {
    /* No game ID from loader = launched from XMB, assume XMB mode */
    USB_LOG("No game ID from loader, assuming XMB mode");
    strcpy(g_startup_game_id, "XMB");
    strcpy(g_startup_game_title, "Browsing XMB");
    strncpy(g_last_game_id, "XMB", sizeof(g_last_game_id) - 1);
    g_game_start_time = sceKernelGetSystemTimeLow() / 1000000;

    /* Wait for USB connection to send XMB info */
    int connect_wait = 0;
    while (!usb_driver_is_connected() && connect_wait < 50) {
      sceKernelDelayThread(100 * 1000);
      connect_wait++;
    }

    if (usb_driver_is_connected()) {
      sceKernelDelayThread(2000 * 1000); /* Wait for stabilization */
      int ret = usb_send_game_info("XMB", "Browsing XMB", 0,
                                   0, /* state = 0, has_icon = 0 */
                                   g_game_start_time, 0, g_config.psp_name);
      if (ret >= 0) {
        USB_LOG("Sent XMB info successfully");
        g_last_game_update = sceKernelGetSystemTimeLow() / 1000000;
      } else {
        USB_LOG_ERR("Failed to send XMB info", ret);
      }
    }
  }

  while (g_running) {
    loop_count++;

    /* Log every 10 loops to show thread is alive */
    if (loop_count <= 3 || loop_count % 10 == 0) {
      USB_LOG("Thread loop iteration");
    }

    /* Get current time in seconds */
    now = sceKernelGetSystemTimeLow() / 1000000;

    /* Check USB connection */
    if (!usb_driver_is_connected()) {
      if (connected_logged) {
        /* Was connected, now disconnected */
        USB_LOG("Host disconnected");
        connected_logged = 0;
      }
      sceKernelDelayThread(500 * 1000); /* 500ms */
      continue;
    }

    if (!connected_logged) {
      USB_LOG("Host connected, starting game detection");
      connected_logged = 1;

      /* Reset stats sync state on new connection */
      g_stats_sync_pending = 0;
      g_stats_sync_done = 0;
      g_last_stats_sync = 0;
    }

    /* Poll for game changes */
    if ((now - last_poll) >= (g_config.poll_interval_ms / 1000)) {
      last_poll = now;
      USB_LOG("Polling for game...");

      memset(&game, 0, sizeof(game));
      int detect_ret = game_detect_current(&game);

      if (detect_ret == 0 && game.game_id[0] != '\0') {
        /* Normalize XMB detection (like net plugin does) */
        if (strncmp(game.game_id, "Xmb", 3) == 0 ||
            strcmp(game.game_id, "XMB") == 0 ||
            strncmp(game.game_id, "SystemCon", 9) == 0 ||
            strcmp(game.game_id, "SystemControl") == 0) {
          strcpy(game.game_id, "XMB");
          strcpy(game.title, "Browsing XMB");
        }

        /* Check if game changed */
        if (strcmp(g_last_game_id, game.game_id) != 0) {
          USB_LOG("Game changed, sending info");

          /* Update tracking */
          strncpy(g_last_game_id, game.game_id, sizeof(g_last_game_id) - 1);
          g_game_start_time = game.start_time;

          /* Send game info */
          int ret = usb_send_game_info(game.game_id, game.title, game.state,
                                       game.has_icon, game.start_time,
                                       0, /* not persistent */
                                       g_config.psp_name);

          if (ret >= 0) {
            USB_LOG("Sent game info successfully");
            g_last_game_update = now;

            /* send_once mode: send one update then exit */
            if (g_config.send_once) {
              USB_LOG("send_once mode: update sent, exiting");
              sceKernelDelayThread(1000 *
                                   1000); /* 1 second for data to flush */
              g_running = 0;
            }
          } else {
            USB_LOG_ERR("Failed to send game info", ret);
          }
        } else {
          /* Game unchanged - check for periodic resend */
          if (g_config.game_update_interval_ms > 0 &&
              (now - g_last_game_update) >=
                  (g_config.game_update_interval_ms / 1000)) {
            USB_LOG("Periodic game update resend");
            int ret = usb_send_game_info(game.game_id, game.title, game.state,
                                         game.has_icon, game.start_time, 0,
                                         g_config.psp_name);
            if (ret >= 0) {
              g_last_game_update = now;
            }
          }
        }
      } else {
        /* Detection failed - log on first failure */
        static int detection_failed_logged = 0;
        if (!detection_failed_logged) {
          if (detect_ret != 0) {
            USB_LOG_ERR("Game detection returned error", detect_ret);
          } else {
            USB_LOG("Game detection returned empty game_id");
          }
          detection_failed_logged = 1;
        }
      }
    }

    /* Send heartbeat */
    if ((now - g_last_heartbeat) >= (g_config.heartbeat_interval_ms / 1000)) {
      g_last_heartbeat = now;
      usb_send_heartbeat(now, 100); /* 100% battery placeholder */
    }

    /* Poll for incoming messages from desktop (icons, stats responses, etc) */
    /* This MUST run every iteration to receive stats response packets! */
    char requested_game_id[10] = {0};
    int msg = usb_poll_message(requested_game_id);

    /* Handle icon requests if enabled */
    if (g_config.send_icons && msg == USB_PKT_ICON_REQUEST &&
        requested_game_id[0] != '\0') {
      USB_LOG("Icon requested, sending...");
      /* Use game_detect_get_icon to get icon data into buffer */
      static uint8_t icon_buffer[32768]; /* Max 32KB icon */
      uint32_t icon_size = 0;
      int ret = game_detect_get_icon(requested_game_id, icon_buffer,
                                     sizeof(icon_buffer), &icon_size);
      if (ret == 0 && icon_size > 0) {
        ret = usb_send_icon(requested_game_id, icon_buffer, icon_size);
        if (ret >= 0) {
          USB_LOG("Icon sent successfully");
        } else {
          USB_LOG_ERR("Failed to send icon", ret);
        }
      } else {
        USB_LOG_ERR("Failed to get icon data", ret);
      }
    }

    /* Stats sync logic - USB mode just syncs with Desktop (no local tracking)
     */
    if (!g_stats_sync_pending) {
      /* Check if we should initiate a sync */
      int should_sync = 0;

      /* Initial sync on first connection */
      if (!g_stats_sync_done && g_last_stats_sync == 0) {
        should_sync = 1;
        USB_LOG("Triggering initial stats sync");
      }
      /* Periodic sync every STATS_SYNC_INTERVAL_S seconds */
      else if (g_stats_sync_done &&
               (now - g_last_stats_sync >= STATS_SYNC_INTERVAL_S)) {
        should_sync = 1;
        USB_LOG("Triggering periodic stats sync");
      }

      if (should_sync) {
        /* Read local timestamp from usage.json (if exists) */
        uint64_t local_ts = read_local_timestamp();
        int ret = usb_send_stats_request(local_ts);
        if (ret >= 0) {
          /* ret > 0 means bytes sent successfully */
          g_stats_sync_pending = 1;
          g_last_stats_sync = now;
          USB_LOG("Stats request sent");
        } else {
          USB_LOG_ERR("Stats request failed", ret);
        }
      }
    } else {
      /* Poll for stats response from Desktop (data streamed directly to file)
       */
      uint64_t remote_ts = 0;
      size_t bytes_received = 0;

      int resp = usb_poll_stats_response(&remote_ts, NULL, 0, &bytes_received);
      if (resp == 1) {
        /* Response complete and verified */
        USB_LOG("Stats sync complete");
        USB_LOG_ERR("Bytes received", (int)bytes_received);
        g_stats_sync_pending = 0;
        g_stats_sync_done = 1;
      } else if (resp == -2) {
        /* Truncated - verification failed, file deleted */
        USB_LOG("Stats sync FAILED - data truncated, will retry");
        USB_LOG_ERR("Bytes received", (int)bytes_received);
        g_stats_sync_pending = 0;
        /* Don't set g_stats_sync_done so it will retry next interval */
      } else if (resp < 0 && resp != -1) {
        /* Other error (but -1 means still waiting) */
        USB_LOG_ERR("Stats response error", resp);
        g_stats_sync_pending = 0;
      }
      /* resp == 0 means still receiving chunks - continue waiting */
    }

    /* Sleep - faster when stats pending to catch incoming chunks */
    if (g_stats_sync_pending) {
      sceKernelDelayThread(10 * 1000); /* 10ms when waiting for stats */
    } else {
      USB_LOG("Thread sleeping...");
      sceKernelDelayThread(100 * 1000); /* 100ms normally */
    }
  }

  USB_LOG("USB thread exiting");
  return 0;
}

/*============================================================================
 * Module Entry Points
 *============================================================================*/

int module_start(SceSize args, void *argp) {
  int ret;

  /* Load config from INI file first (before any logging) */
  usb_config_load(&g_config);
  g_logging_enabled = g_config.enable_logging;

  USB_LOG("USB module starting...");

  /* Check if plugin is enabled */
  if (!g_config.enabled) {
    USB_LOG("Plugin disabled in config, exiting");
    return 1; /* Non-zero = don't keep module loaded */
  }

  /* Check for startup args with game info from loader */
  g_startup_game_id[0] = '\0';
  g_startup_game_title[0] = '\0';
  if (args >= sizeof(UsbStartupArgs) && argp != NULL) {
    UsbStartupArgs *startup = (UsbStartupArgs *)argp;
    if (startup->magic == USB_STARTUP_MAGIC) {
      strncpy(g_startup_game_id, startup->game_id,
              sizeof(g_startup_game_id) - 1);
      g_startup_game_id[sizeof(g_startup_game_id) - 1] = '\0';
      USB_LOG("Received game ID from loader");

      if (startup->game_title[0] != '\0') {
        strncpy(g_startup_game_title, startup->game_title,
                sizeof(g_startup_game_title) - 1);
        g_startup_game_title[sizeof(g_startup_game_title) - 1] = '\0';
        USB_LOG("Received game title from loader");
      }
    }
  }

  USB_LOG("Config loaded, starting USB driver");

  /* Vblank wait before USB init (per-game override if available) */
  if (g_config.vblank_wait > 0) {
    int wait_count = (int)g_config.vblank_wait;

    /* Check for per-game override */
    if (g_startup_game_id[0] != '\0') {
      wait_count = usb_config_get_game_vblank_wait(g_startup_game_id,
                                                   g_config.vblank_wait);
    }

    if (wait_count > 0) {
      g_running = 1; /* For vblank wait loop */
      wait_for_vblanks(wait_count);
    }
  }

  /* Initialize USB driver */
  ret = usb_driver_init();
  if (ret < 0) {
    USB_LOG_ERR("USB driver init failed", ret);
    return 1;
  }

  /* Start USB driver */
  ret = usb_driver_start();
  if (ret < 0) {
    USB_LOG_ERR("USB driver start failed", ret);
    usb_driver_shutdown();
    return 1;
  }

  /* Create and start main thread */
  g_running = 1;
  g_thread_id = sceKernelCreateThread(
      "PSPDRP_USB_Thread", usb_main_thread, 0x11, /* priority */
      0x4000,                                     /* stack size 16KB */
      0x00800000, /* attributes: no kill on module exit */
      NULL);

  if (g_thread_id >= 0) {
    sceKernelStartThread(g_thread_id, 0, NULL);
    USB_LOG("USB thread created");
  } else {
    USB_LOG_ERR("Failed to create USB thread", g_thread_id);
  }

  USB_LOG("USB module started");
  return 0;
}

int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  USB_LOG("USB module stopping...");

  /* Stop thread */
  g_running = 0;
  if (g_thread_id >= 0) {
    sceKernelWaitThreadEnd(g_thread_id, NULL);
    sceKernelDeleteThread(g_thread_id);
    g_thread_id = -1;
  }

  /* Shutdown USB */
  usb_driver_shutdown();

  USB_LOG("USB module stopped");
  return 0;
}
