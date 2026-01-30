/**
 * PSP DRP Net Plugin
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <pspwlan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "discord_rpc.h"
#include "game_detect.h"
#include "network.h"

PSP_MODULE_INFO("PSPDRP_Net", PSP_MODULE_USER, 1, 0);

#define LOG_PATH "ms0:/psp_drp.log"

#define RPC_START_MAGIC 0x31504352
#define RPC_START_FLAG_FROM_UI 0x01

typedef struct {
  unsigned int magic;
  int profile_id;
  unsigned int flags;
} RpcStartArgs;

/* Plugin configuration */
static PluginConfig g_config;

/* Current game state */
static GameInfo g_current_game;
static int g_game_changed = 0;

/* Network state */
static int g_network_initialized = 0;
static int g_connected = 0;
static int g_waiting_for_ack = 0;

/* Thread control */
static SceUID g_main_thread = -1;
static volatile int g_running = 1;

/* Start arguments */
static int g_profile_id = 1;
static int g_started_from_ui = 0;

/* Mode label for logs */
static const char *g_mode_label = "UNK";

/* Heartbeat timing */
static SceUInt64 g_last_heartbeat = 0;
static SceUInt64 g_last_game_check = 0;
static SceUInt64 g_last_game_send = 0;
static SceUInt64 g_last_connect_attempt = 0;
static int g_init_attempts = 0;
static int g_connect_attempts = 0;
static SceUInt64 g_connect_start_us = 0;

#define ICON_BUFFER_SIZE (256 * 1024)
static uint8_t g_icon_buffer[ICON_BUFFER_SIZE];
static char g_last_icon_id[10] = {0};

#define HEARTBEAT_INTERVAL_US (30 * 1000 * 1000) /* 30 seconds */
#define GAME_CHECK_INTERVAL_US (2 * 1000 * 1000) /* 2 seconds */
#define CONNECT_RETRY_US (5 * 1000 * 1000)       /* 5 seconds */

/* Logging enable flag (set from config after load) */
static int g_logging_enabled = 0;

void net_log(const char *fmt, ...) {
  char buf[128];
  char line[160];
  va_list args;
  SceUID fd;
  int len;

  /* Skip if logging is disabled */
  if (!g_logging_enabled) {
    return;
  }

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) {
    return;
  }

  len = (int)strlen(buf);
  if (len > 0) {
    if (g_mode_label != NULL) {
      snprintf(line, sizeof(line), "[NET:%s] %s", g_mode_label, buf);
      sceIoWrite(fd, line, (int)strlen(line));
    } else {
      snprintf(line, sizeof(line), "[NET] %s", buf);
      sceIoWrite(fd, line, (int)strlen(line));
    }
    sceIoWrite(fd, "\n", 1);
  }
  sceIoClose(fd);
}

/**
 * Get current time in microseconds
 */
static SceUInt64 get_time_us(void) {
  SceKernelSysClock clock;
  sceKernelGetSystemTime(&clock);
  return clock.low + ((SceUInt64)clock.hi << 32);
}

static int plugin_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  GameInfo new_game;
  SceUInt64 now;
  char early_game_id[10] = {0};
  int game_specific_delay = -1;
  int network_init_failed = 0;

  net_log("Net thread started");

  if (config_load(&g_config) < 0) {
    config_set_defaults(&g_config);
  }

  /* Set logging flag from config */
  g_logging_enabled = g_config.enable_logging;

  net_log("Config: enabled=%d ip=%s port=%d auto=%d always=%d icons=%d "
          "poll_ms=%lu hb_ms=%lu update_ms=%lu timeout_s=%lu send_once=%d",
          g_config.enabled, g_config.desktop_ip, g_config.port,
          g_config.auto_discovery, g_config.always_active, g_config.send_icons,
          (unsigned long)g_config.poll_interval_ms,
          (unsigned long)g_config.heartbeat_interval_ms,
          (unsigned long)g_config.game_update_interval_ms,
          (unsigned long)g_config.connect_timeout_s, g_config.send_once);

  if (!g_config.enabled) {
    net_log("Plugin disabled in config");
    return 0;
  }

  game_detect_init();

  /* Early game detection to check for game-specific startup delay */
  if (!g_started_from_ui) {
    if (game_detect_current(&new_game) == 0 && new_game.game_id[0] != '\0') {
      strncpy(early_game_id, new_game.game_id, sizeof(early_game_id) - 1);
      net_log("Early game detect: %s", early_game_id);

      /* Check for game-specific startup delay - apply BEFORE network init */
      game_specific_delay = config_get_game_startup_delay(early_game_id);
      net_log("Game delay lookup for %s: %d", early_game_id,
              game_specific_delay);
      if (game_specific_delay >= 0) {
        net_log("Applying game-specific delay: %d ms", game_specific_delay);
        sceKernelDelayThread(game_specific_delay * 1000);
      }
    }
  }

  /* Now try to initialize network (after any game-specific delay) */
  if (!g_network_initialized && !g_started_from_ui) {
    net_log("Attempting network init");
    int early_init = network_init();
    if (early_init == 0) {
      g_network_initialized = 1;
      net_log("Network init SUCCESS");
    } else {
      net_log("Network init failed: 0x%08X", (unsigned int)early_init);
    }
  }

  if (g_started_from_ui) {
    memset(&g_current_game, 0, sizeof(g_current_game));
    strcpy(g_current_game.game_id, "XMB");
    strcpy(g_current_game.title, "Browsing XMB");
    g_current_game.state = STATE_XMB;
    g_current_game.start_time = 0;
    g_current_game.has_icon = 0;
    g_game_changed = 1;
  }

  while (g_running) {
    now = get_time_us();

    if (!g_network_initialized && sceWlanGetSwitchState() == 1) {
      g_init_attempts++;
      net_log("network_init attempt=%d", g_init_attempts);
      int net_res = network_init();
      if (net_res == 0) {
        g_network_initialized = 1;
        network_init_failed = 0; /* Reset failure flag on success */

        g_last_connect_attempt = now;
        g_connect_attempts++;
        net_log("network_connect attempt=%d", g_connect_attempts);
        if (g_connect_start_us == 0) {
          g_connect_start_us = now;
        }
        {
          int conn_res = network_connect(&g_config);
          if (conn_res == 0) {
            g_connected = 0;
            g_waiting_for_ack = 1;
            net_log("Waiting for ACK");
          } else if (conn_res == 1) {
            g_connected = 0;
            g_waiting_for_ack = 1;
            net_log("Waiting for discovery");
          } else {
            g_connected = 0;
            net_log("network_connect failed: %d", conn_res);
          }
        }
      } else {
        net_log("network_init failed: 0x%08X", (unsigned int)net_res);

        /* On first failure, try force cleanup to take over from game */
        if (g_init_attempts == 1) {
          net_log("First failure, attempting force cleanup");
          network_force_cleanup();
        }

        /* After 10 failed attempts, write a placeholder for this game */
        if (g_init_attempts >= 10 && !network_init_failed &&
            early_game_id[0] != '\0') {
          network_init_failed = 1;
          net_log("Writing game delay placeholder for: %s", early_game_id);
          config_write_game_delay_placeholder(early_game_id);
        }

        sceKernelDelayThread(2000 * 1000);
      }
    }

    if (g_network_initialized && sceWlanGetSwitchState() == 0) {
      network_disconnect();
      network_shutdown();
      g_network_initialized = 0;
      g_connected = 0;
    }

    if (g_network_initialized && g_config.auto_discovery) {
      int found = network_handle_discovery(&g_config);
      if (found > 0) {
        g_connected = 1;
        g_waiting_for_ack = 0;
        g_connect_start_us = 0;
        net_log("Discovered %s:%d", g_config.desktop_ip, g_config.port);
      }
    }

    /* Poll for incoming messages (ACK or icon request) */
    if (g_network_initialized) {
      char requested_game_id[10] = {0};
      int msg_result = network_poll_message(requested_game_id);

      if (msg_result == 1 && g_waiting_for_ack) {
        /* ACK received */
        g_connected = 1;
        g_waiting_for_ack = 0;
        g_connect_start_us = 0;
        net_log("Desktop ACK received");
      } else if (msg_result == 2 && g_connected && g_config.send_icons) {
        /* Icon request received */
        net_log("Icon requested for: %s", requested_game_id);
        uint32_t icon_size = 0;
        int icon_res = game_detect_get_icon(requested_game_id, g_icon_buffer,
                                            ICON_BUFFER_SIZE, &icon_size);
        if (icon_res == 0) {
          if (network_send_icon(requested_game_id, g_icon_buffer, icon_size) ==
              0) {
            net_log("Icon sent on request (%u bytes)", (unsigned int)icon_size);
          } else {
            net_log("Icon send failed for request");
          }
        } else {
          net_log("Icon load failed for request: %d", icon_res);
        }
      }
    }

    if (g_network_initialized && !g_connected && !g_config.auto_discovery) {
      if (now - g_last_connect_attempt >= CONNECT_RETRY_US) {
        g_last_connect_attempt = now;
        g_connect_attempts++;
        net_log("network_connect attempt=%d", g_connect_attempts);
        if (g_connect_start_us == 0) {
          g_connect_start_us = now;
        }
        {
          int conn_res = network_connect(&g_config);
          if (conn_res == 0) {
            g_connected = 0;
            g_waiting_for_ack = 1;
            net_log("Waiting for ACK");
          } else if (conn_res == 1) {
            g_connected = 0;
            g_waiting_for_ack = 1;
            net_log("Waiting for discovery");
          } else {
            g_connected = 0;
            net_log("network_connect failed: %d", conn_res);
          }
        }
      }
    }

    if (!g_connected && g_connect_start_us != 0 &&
        g_config.connect_timeout_s > 0) {
      SceUInt64 timeout_us = (SceUInt64)g_config.connect_timeout_s * 1000000;
      if (now - g_connect_start_us >= timeout_us) {
        net_log("Connect timeout reached (%u s), deactivating",
                (unsigned int)g_config.connect_timeout_s);
        g_running = 0;
        if (g_network_initialized) {
          network_disconnect();
          network_shutdown();
          g_network_initialized = 0;
          g_connected = 0;
        }
        break;
      }
    }

    if (!g_started_from_ui) {
      SceUInt64 game_check_interval_us =
          (SceUInt64)g_config.poll_interval_ms * 1000;
      if (game_check_interval_us < 500000) {
        game_check_interval_us = 500000;
      }
      if (now - g_last_game_check >= game_check_interval_us) {
        g_last_game_check = now;
        if (game_detect_current(&new_game) == 0) {
          if (strcmp(new_game.game_id, g_current_game.game_id) != 0 ||
              new_game.state != g_current_game.state) {
            memcpy(&g_current_game, &new_game, sizeof(GameInfo));
            g_game_changed = 1;
          }
        }
      }
    }

    if (g_network_initialized) {
      {
        SceUInt64 heartbeat_interval_us =
            (SceUInt64)g_config.heartbeat_interval_ms * 1000;
        if (heartbeat_interval_us < 1000000) {
          heartbeat_interval_us = 1000000;
        }
        if (heartbeat_interval_us > 300000000) {
          heartbeat_interval_us = 300000000;
        }
        if (now - g_last_heartbeat >= heartbeat_interval_us) {
          g_last_heartbeat = now;
          network_send_heartbeat();
        }
      }

      if (g_connected) {
        int should_send = g_game_changed;
        if (!should_send && g_config.game_update_interval_ms > 0) {
          SceUInt64 resend_us =
              (SceUInt64)g_config.game_update_interval_ms * 1000;
          if (resend_us < 1000000) {
            resend_us = 1000000;
          }
          if (resend_us > 3600000000ULL) {
            resend_us = 3600000000ULL;
          }
          if (now - g_last_game_send >= resend_us) {
            should_send = 1;
          }
        }

        if (should_send) {
          int icon_needed = g_game_changed;
          /* Set persistent flag for send_once mode */
          g_current_game.persistent = g_config.send_once ? 1 : 0;
          /* Copy PSP name from config */
          memcpy(g_current_game.psp_name, g_config.psp_name,
                 sizeof(g_current_game.psp_name));
          if (network_send_game_info(&g_current_game) >= 0) {
            if (icon_needed && g_config.send_icons && g_current_game.has_icon) {
              if (strncmp(g_last_icon_id, g_current_game.game_id,
                          sizeof(g_last_icon_id)) != 0) {
                uint32_t icon_size = 0;
                int icon_res =
                    game_detect_get_icon(g_current_game.game_id, g_icon_buffer,
                                         ICON_BUFFER_SIZE, &icon_size);
                if (icon_res == 0) {
                  if (network_send_icon(g_current_game.game_id, g_icon_buffer,
                                        icon_size) == 0) {
                    memcpy(g_last_icon_id, g_current_game.game_id,
                           sizeof(g_last_icon_id));
                    net_log("Icon sent (%u bytes)", (unsigned int)icon_size);
                  } else {
                    net_log("Icon send failed");
                  }
                } else {
                  net_log("Icon load failed: %d", icon_res);
                }
              }
            }
            g_game_changed = 0;
            g_last_game_send = now;

            /* Send once mode: after first successful send, shutdown and exit */
            if (g_config.send_once) {
              net_log("Send once complete, shutting down network");
              network_disconnect();
              network_shutdown();
              g_network_initialized = 0;
              g_connected = 0;
              g_running = 0;
              break;
            }
          }
        }
      }
    }

    sceKernelDelayThread(100 * 1000);
  }

  return 0;
}

int module_start(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  if (argp != NULL && args >= (SceSize)sizeof(RpcStartArgs)) {
    RpcStartArgs *start = (RpcStartArgs *)argp;
    if (start->magic == RPC_START_MAGIC) {
      if (start->profile_id > 0) {
        g_profile_id = start->profile_id;
      }
      g_started_from_ui = (start->flags & RPC_START_FLAG_FROM_UI) ? 1 : 0;
    }
  }

  g_mode_label = g_started_from_ui ? "VSH" : "GAME";

  net_log("module_start called");
  if (argp != NULL && args >= (SceSize)sizeof(RpcStartArgs)) {
    RpcStartArgs *start = (RpcStartArgs *)argp;
    if (start->magic == RPC_START_MAGIC) {
      net_log("Start args profile=%d flags=0x%X", g_profile_id, start->flags);
    }
  }

  network_set_profile_id(g_profile_id);

  g_main_thread = sceKernelCreateThread("PSPDRP_Net", plugin_thread, 0x11,
                                        0x4000, PSP_THREAD_ATTR_USER, NULL);

  if (g_main_thread >= 0) {
    sceKernelStartThread(g_main_thread, 0, NULL);
  } else {
    net_log("Thread create failed: %d", g_main_thread);
  }

  return 0;
}

int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  net_log("module_stop called");
  g_running = 0;

  if (!g_started_from_ui) {
    net_log("Skipping cleanup in GAME");
    return 0;
  }

  if (g_main_thread >= 0) {
    sceKernelWaitThreadEnd(g_main_thread, NULL);
    sceKernelDeleteThread(g_main_thread);
  }

  if (g_connected) {
    network_disconnect();
    g_connected = 0;
  }
  if (g_network_initialized) {
    network_shutdown();
    g_network_initialized = 0;
  }

  return 0;
}
