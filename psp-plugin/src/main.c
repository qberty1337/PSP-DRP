/**
 * PSP DRP Plugin
 *
 * A VSH plugin for ARK 4 CFW that sends game/app information
 * to a desktop companion app for Discord Rich Presence display.
 *
 * For PSP-1000 on CFW 6.61 ARK 4
 */

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psppower.h>
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

PSP_MODULE_INFO("PSPDRP", 0x1007, 1, 0);

/* Plugin configuration */
static PluginConfig g_config;

/* Current game state */
static GameInfo g_current_game;
static int g_game_changed = 0;

/* Network state */
static int g_network_initialized = 0;
static int g_connected = 0;

/* Thread control */
static SceUID g_main_thread = -1;
static volatile int g_running = 1;
static volatile int g_active = 1;

/* Input state */
static unsigned int g_prev_buttons = 0;

/* Heartbeat timing */
static SceUInt64 g_last_heartbeat = 0;
static SceUInt64 g_last_game_check = 0;

#define HEARTBEAT_INTERVAL_US (30 * 1000 * 1000) /* 30 seconds */
#define GAME_CHECK_INTERVAL_US (2 * 1000 * 1000) /* 2 seconds */

/* Debug overlay */
#define DEBUG_LINE_COUNT 12
#define DEBUG_LINE_LEN 64
#define DEBUG_LOG_PATH "ms0:/psp_drp.log"
static int g_debug_inited = 0;
static int g_debug_dirty = 0;
static int g_debug_line_count = 0;
static char g_debug_lines[DEBUG_LINE_COUNT][DEBUG_LINE_LEN];

static void debug_init(void) {
  if (g_debug_inited) {
    return;
  }
  pspDebugScreenInit();
  pspDebugScreenClear();
  g_debug_inited = 1;
}

static void debug_log(const char *fmt, ...) {
  va_list args;
  char buf[DEBUG_LINE_LEN];
  int i;

  if (!g_debug_inited) {
    return;
  }

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (g_debug_line_count < DEBUG_LINE_COUNT) {
    snprintf(g_debug_lines[g_debug_line_count], DEBUG_LINE_LEN, "%s", buf);
    g_debug_line_count++;
  } else {
    for (i = 1; i < DEBUG_LINE_COUNT; i++) {
      strcpy(g_debug_lines[i - 1], g_debug_lines[i]);
    }
    snprintf(g_debug_lines[DEBUG_LINE_COUNT - 1], DEBUG_LINE_LEN, "%s", buf);
  }

  g_debug_dirty = 1;
}

static void debug_file_log(const char *fmt, ...) {
  char buf[128];
  va_list args;
  SceUID fd;
  int len;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  fd = sceIoOpen(DEBUG_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND,
                 0777);
  if (fd < 0) {
    return;
  }

  len = (int)strlen(buf);
  if (len > 0) {
    sceIoWrite(fd, buf, len);
    sceIoWrite(fd, "\n", 1);
  }
  sceIoClose(fd);
}

static void debug_file_log_raw(const char *msg) {
  SceUID fd;
  int len = 0;

  if (msg == NULL) {
    return;
  }

  while (msg[len] != '\0') {
    len++;
  }

  fd = sceIoOpen(DEBUG_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND,
                 0777);
  if (fd < 0) {
    return;
  }

  if (len > 0) {
    sceIoWrite(fd, msg, len);
    sceIoWrite(fd, "\n", 1);
  }
  sceIoClose(fd);
}

static void debug_draw(void) {
  int i;
  if (!g_debug_inited || !g_debug_dirty) {
    return;
  }

  pspDebugScreenSetXY(0, 0);
  pspDebugScreenClear();
  for (i = 0; i < g_debug_line_count; i++) {
    pspDebugScreenPrintf("%s\n", g_debug_lines[i]);
  }
  g_debug_dirty = 0;
}

/**
 * Get current time in microseconds
 */
static SceUInt64 get_time_us(void) {
  SceKernelSysClock clock;
  sceKernelGetSystemTime(&clock);
  return clock.low + ((SceUInt64)clock.hi << 32);
}

/**
 * Main plugin thread
 */
static int plugin_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  GameInfo new_game;
  SceUInt64 now;
  SceCtrlData pad;
  const unsigned int toggle_combo = PSP_CTRL_LTRIGGER | PSP_CTRL_SELECT;

  /* Wait for system to fully boot */
  sceKernelDelayThread(5 * 1000 * 1000);

  /* Load configuration */
  if (config_load(&g_config) < 0) {
    /* Use defaults if config fails */
    config_set_defaults(&g_config);
  }

  /* Check if plugin is enabled */
  if (!g_config.enabled) {
    return 0;
  }

  debug_init();
  debug_log("PSPDRP loaded");
  debug_log("L+SELECT: WiFi connect");
  debug_log("WLAN switch: %d", sceWlanGetSwitchState());
  debug_log("Thread running");
  debug_file_log("PSPDRP loaded");
  debug_file_log("WLAN switch: %d", sceWlanGetSwitchState());

  g_active = g_config.enabled ? 1 : 0;

  /* Init controller sampling for hotkey */
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

  /* Initialize game detection */
  game_detect_init();

  /* Main loop */
  while (g_running) {
    now = get_time_us();

    /* Read controller for hotkey toggle */
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
      if ((pad.Buttons & toggle_combo) == toggle_combo &&
          (g_prev_buttons & toggle_combo) != toggle_combo) {
        debug_log("Hotkey pressed");
        debug_file_log("Hotkey pressed");

        g_active = 1;

        if (!g_network_initialized) {
          if (network_init() == 0) {
            g_network_initialized = 1;
          } else {
            debug_log("net init failed");
            debug_file_log("net init failed");
            g_active = 0;
          }
        }

        if (g_active) {
          debug_log("Opening WiFi selector");
          debug_file_log("Opening WiFi selector");
          if (network_show_profile_selector() < 0) {
            debug_log("WiFi selector canceled");
            debug_file_log("WiFi selector canceled");
            g_active = 0;
            if (g_network_initialized) {
              network_shutdown();
              g_network_initialized = 0;
            }
            g_connected = 0;
          } else {
            int conn_result;
            debug_log("WiFi connected");
            debug_file_log("WiFi connected");
            if (g_connected) {
              network_disconnect();
              g_connected = 0;
            }
            conn_result = network_connect(&g_config);
            if (conn_result == 0) {
              g_connected = 1;
              debug_log("Desktop connected");
              debug_file_log("Desktop connected");
            } else if (conn_result == 1) {
              g_connected = 0;
              debug_log("Waiting for discovery");
              debug_file_log("Waiting for discovery");
            } else {
              g_connected = 0;
              debug_log("Desktop connect failed");
              debug_file_log("Desktop connect failed");
            }
          }
        }
      }
      g_prev_buttons = pad.Buttons;
    }

    if (!g_active) {
      debug_draw();
      sceKernelDelayThread(100 * 1000);
      continue;
    }

    /* Initialize network if WiFi is available */
    if (!g_network_initialized && sceWlanGetSwitchState() == 1) {
      if (network_init() == 0) {
        g_network_initialized = 1;

        /* Connect to desktop app */
        if (network_connect(&g_config) == 0) {
          g_connected = 1;
        } else {
          g_connected = 0;
        }
      }
    }

    /* Handle WiFi being turned off */
    if (g_network_initialized && sceWlanGetSwitchState() == 0) {
      network_disconnect();
      network_shutdown();
      g_network_initialized = 0;
      g_connected = 0;
    }

    /* Auto-discovery handling */
    if (g_network_initialized && g_config.auto_discovery) {
      int found = network_handle_discovery(&g_config);
      if (found > 0) {
        g_connected = 1;
        debug_log("Discovered %s:%d", g_config.desktop_ip, g_config.port);
        debug_file_log("Discovered %s:%d", g_config.desktop_ip, g_config.port);
      }
    }

    /* Check for game changes */
    if (now - g_last_game_check >= GAME_CHECK_INTERVAL_US) {
      g_last_game_check = now;

      if (game_detect_current(&new_game) == 0) {
        /* Check if game changed */
        if (strcmp(new_game.game_id, g_current_game.game_id) != 0 ||
            new_game.state != g_current_game.state) {
          memcpy(&g_current_game, &new_game, sizeof(GameInfo));
          g_game_changed = 1;
        }
      }
    }

    /* Send updates if connected */
    if (g_connected) {
      /* Send game info if changed */
      if (g_game_changed) {
        if (network_send_game_info(&g_current_game) == 0) {
          g_game_changed = 0;
        }
      }

      /* Send heartbeat periodically */
      if (now - g_last_heartbeat >= HEARTBEAT_INTERVAL_US) {
        g_last_heartbeat = now;
        network_send_heartbeat();
      }
    }

    /* Sleep to avoid busy-waiting */
    debug_draw();
    sceKernelDelayThread(100 * 1000); /* 100ms */
  }

  return 0;
}

/**
 * Module start - called when plugin loads
 */
int module_start(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  debug_file_log_raw("module_start called");
  debug_file_log("module_start called");

  /* Create main thread */
  g_main_thread =
      sceKernelCreateThread("PSPDRP_Main", plugin_thread,
                            0x11,   /* Priority (lower = higher priority) */
                            0x4000, /* Stack size: 16KB */
                            PSP_THREAD_ATTR_USER, /* Attributes */
                            NULL                  /* Options */
      );

  if (g_main_thread >= 0) {
    sceKernelStartThread(g_main_thread, 0, NULL);
  } else {
    debug_file_log("Thread create failed: %d", g_main_thread);
  }

  return 0;
}

/**
 * Module stop - called when plugin unloads
 */
int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  g_running = 0;
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
