#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>

#include "draw.h"

PSP_MODULE_INFO("PSPDRP_UI", PSP_MODULE_USER, 1, 0);

#define UI_LOG_PATH "ms0:/psp_drp.log"
#define UI_LOG_PREFIX "[UI] "
#define NET_PRX_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp_net.prx"

#define RPC_START_MAGIC 0x31504352
#define RPC_START_FLAG_FROM_UI 0x01

typedef struct {
  unsigned int magic;
  int profile_id;
  unsigned int flags;
} RpcStartArgs;

#define MAX_PROFILES 16
#define MAX_NAME_LEN 64

#define COLOR_BG 0xFF101018
#define COLOR_PANEL 0xFF1A1A22
#define COLOR_BORDER 0xFF2F2F3A
#define COLOR_TEXT 0xFFFFFFFF
#define COLOR_MUTED 0xFFB0B0B8
#define COLOR_HILITE 0xFF2C5F9E
#define COLOR_ACCENT 0xFF89C2FF

static void ui_log_raw(const char *msg) {
  SceUID fd;
  int len = 0;
  int prefix_len = 0;

  if (msg == NULL) {
    return;
  }

  while (msg[len] != '\0') {
    len++;
  }

  while (UI_LOG_PREFIX[prefix_len] != '\0') {
    prefix_len++;
  }

  fd = sceIoOpen(UI_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) {
    return;
  }

  if (len > 0) {
    if (prefix_len > 0) {
      sceIoWrite(fd, UI_LOG_PREFIX, prefix_len);
    }
    sceIoWrite(fd, msg, len);
    sceIoWrite(fd, "\n", 1);
  }
  sceIoClose(fd);
}

static int ui_thread(SceSize args, void *argp);

static void *g_saved_fb = NULL;
static int g_saved_stride = 0;
static int g_saved_pixfmt = 0;
static int g_saved_valid = 0;

typedef struct {
  int id;
  char name[MAX_NAME_LEN];
} WifiProfile;

static void copy_str(char *dst, int dst_size, const char *src) {
  int len;
  if (dst == NULL || dst_size <= 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  len = (int)strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  if (len > 0) {
    memcpy(dst, src, len);
  }
  dst[len] = '\0';
}

static void num_to_str(int value, char *out, int out_size) {
  char buf[16];
  int pos = 0;
  int i;
  if (out == NULL || out_size <= 0) {
    return;
  }
  if (value == 0) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (value > 0 && pos < (int)(sizeof(buf) - 1)) {
    buf[pos++] = (char)('0' + (value % 10));
    value /= 10;
  }
  if (pos >= out_size) {
    pos = out_size - 1;
  }
  for (i = 0; i < pos; i++) {
    out[i] = buf[pos - 1 - i];
  }
  out[pos] = '\0';
}

static void wait_for_button_release(int frames) {
  SceCtrlData pad;
  int i;
  for (i = 0; i < frames; i++) {
    sceCtrlReadBufferPositive(&pad, 1);
    if (pad.Buttons == 0) {
      return;
    }
    sceDisplayWaitVblankStart();
    sceKernelDelayThread(30 * 1000);
  }
}

static void ui_log_hex(const char *label, unsigned int value) {
  char buf[64];
  if (label == NULL) {
    return;
  }
  snprintf(buf, sizeof(buf), "%s0x%08X", label, value);
  ui_log_raw(buf);
}

static void display_save(void) {
  void *fb = NULL;
  int stride = 0;
  int pixfmt = 0;

  if (sceDisplayGetFrameBuf(&fb, &stride, &pixfmt, 0) == 0 && fb != NULL) {
    g_saved_fb = fb;
    g_saved_stride = stride;
    g_saved_pixfmt = pixfmt;
    g_saved_valid = 1;
  }
}

static void display_restore(void) {
  if (!g_saved_valid) {
    return;
  }
  sceDisplayWaitVblankStart();
  sceDisplaySetFrameBuf(g_saved_fb, g_saved_stride, g_saved_pixfmt,
                        PSP_DISPLAY_SETBUF_NEXTFRAME);
  g_saved_valid = 0;
}

static int enum_profiles(WifiProfile *profiles, int max_profiles) {
  int count = 0;
  int i;
  if (profiles == NULL || max_profiles <= 0) {
    return 0;
  }
  for (i = 1; i <= MAX_PROFILES; i++) {
    if (sceUtilityCheckNetParam(i) == 0) {
      char name[MAX_NAME_LEN];
      netData data;
      memset(name, 0, sizeof(name));
      memset(&data, 0, sizeof(data));
      if (sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &data) < 0 ||
          data.asString[0] == '\0') {
        char num[16];
        char fallback[MAX_NAME_LEN];
        num_to_str(i, num, sizeof(num));
        strcpy(fallback, "Profile ");
        copy_str(fallback + 8, sizeof(fallback) - 8, num);
        copy_str(name, sizeof(name), fallback);
      } else {
        copy_str(name, sizeof(name), data.asString);
      }
      profiles[count].id = i;
      copy_str(profiles[count].name, sizeof(profiles[count].name), name);
      count++;
      if (count >= max_profiles) {
        break;
      }
    }
  }
  return count;
}

static void draw_header(const char *title) {
  draw_rect_filled(12, 10, 456, 24, COLOR_PANEL);
  draw_rect(12, 10, 456, 24, COLOR_BORDER);
  draw_text(20, 16, title, COLOR_ACCENT);
}

static int show_profile_menu(int *out_profile_id) {
  WifiProfile profiles[MAX_PROFILES];
  int profile_count;
  int result = -1;
  int selected = 0;
  int top = 0;
  int visible = 12;
  unsigned int confirm_btn = PSP_CTRL_CROSS;
  unsigned int cancel_btn = PSP_CTRL_TRIANGLE;
  const char *cancel_text = "Triangle: cancel";
  SceCtrlData pad;
  unsigned int old_buttons = 0;
  int input_ready = 0;
  int input_cooldown = 15;
  int confirm_cooldown = 30;

  profile_count = enum_profiles(profiles, MAX_PROFILES);

  display_save();

  wait_for_button_release(8);
  sceCtrlReadBufferPositive(&pad, 1);
  old_buttons = pad.Buttons;

  while (1) {
    unsigned int pressed;
    sceDisplayWaitVblankStart();
    if (draw_begin() < 0) {
      ui_log_raw("draw_begin failed");
      break;
    }

    draw_clear(COLOR_BG);
    draw_header("PSP DRP WiFi");

    if (sceWlanGetSwitchState() == 0) {
      draw_text(20, 60, "WLAN switch is off.", COLOR_TEXT);
      draw_text(20, 72, "Turn it on to connect.", COLOR_MUTED);
      draw_text(20, 240, cancel_text, COLOR_MUTED);
    } else if (profile_count == 0) {
      draw_text(20, 60, "No network profiles found.", COLOR_TEXT);
      draw_text(20, 72, "Create one in the PSP network settings.", COLOR_MUTED);
      draw_text(20, 240, cancel_text, COLOR_MUTED);
    } else {
      int i;
      int base_y = 50;
      int line_h = 14;
      int max_index = profile_count - 1;

      if (selected < 0) {
        selected = 0;
      }
      if (selected > max_index) {
        selected = max_index;
      }
      if (selected < top) {
        top = selected;
      }
      if (selected >= top + visible) {
        top = selected - visible + 1;
      }

      draw_text(20, 36, "Select a WLAN profile:", COLOR_MUTED);

      for (i = 0; i < visible; i++) {
        int idx = top + i;
        int y = base_y + i * line_h;
        if (idx >= profile_count) {
          break;
        }
        if (idx == selected) {
          draw_rect_filled(16, y - 2, 448, 12, COLOR_HILITE);
          draw_text(24, y, profiles[idx].name, COLOR_TEXT);
        } else {
          draw_text(24, y, profiles[idx].name, COLOR_TEXT);
        }
      }

      draw_text(20, 240, "X: select  Triangle: cancel", COLOR_MUTED);
    }

    sceCtrlReadBufferPositive(&pad, 1);

    if (input_cooldown > 0) {
      input_cooldown--;
      old_buttons = pad.Buttons;
      continue;
    }

    if (pad.Buttons == 0) {
      input_ready = 1;
    }

    pressed = input_ready ? (pad.Buttons & ~old_buttons) : 0;
    old_buttons = pad.Buttons;

    if (pressed & cancel_btn) {
      ui_log_raw("WiFi menu cancel pressed");
      ui_log_hex("Buttons=", pad.Buttons);
      ui_log_hex("OldButtons=", old_buttons);
      result = -1;
      break;
    }

    if (sceWlanGetSwitchState() == 0 || profile_count == 0) {
      continue;
    }

    if (pressed & PSP_CTRL_UP) {
      selected--;
    }
    if (pressed & PSP_CTRL_DOWN) {
      selected++;
    }
    if (confirm_cooldown > 0) {
      confirm_cooldown--;
    }

    if (confirm_cooldown <= 0 && (pressed & confirm_btn)) {
      if (out_profile_id != NULL) {
        *out_profile_id = profiles[selected].id;
      }
      result = 0;
      break;
    }
  }

  display_restore();
  return result;
}

static int load_net_plugin(int profile_id) {
  SceUID modid = sceKernelLoadModule(NET_PRX_PATH, 0, NULL);
  if (modid < 0) {
    ui_log_raw("Load net PRX failed");
    ui_log_hex("LoadNet=", (unsigned int)modid);
    return -1;
  }
  {
    RpcStartArgs args;
    int start_res;
    memset(&args, 0, sizeof(args));
    args.magic = RPC_START_MAGIC;
    args.profile_id = profile_id;
    args.flags = RPC_START_FLAG_FROM_UI;
    start_res = sceKernelStartModule(modid, sizeof(args), &args, NULL, NULL);
    if (start_res < 0) {
      ui_log_raw("Start net PRX failed");
      ui_log_hex("StartNet=", (unsigned int)start_res);
      sceKernelUnloadModule(modid);
      return -1;
    }
  }
  ui_log_raw("Net PRX started");
  return 0;
}

int module_start(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  ui_log_raw("UI module_start");

  SceUID thid = sceKernelCreateThread("PSPDRP_UI", ui_thread, 0x12, 0x2000,
                                      PSP_THREAD_ATTR_USER, NULL);

  if (thid >= 0) {
    sceKernelStartThread(thid, 0, NULL);
  } else {
    ui_log_raw("UI thread create failed");
  }

  return 0;
}

int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  return 0;
}

static int ui_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  int profile_id = -1;

  ui_log_raw("UI thread start");
  sceKernelDelayThread(500 * 1000);

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

  if (show_profile_menu(&profile_id) < 0) {
    ui_log_raw("WiFi menu canceled");
    sceKernelExitDeleteThread(0);
    return 0;
  }

  ui_log_raw("WiFi profile selected");
  ui_log_hex("Profile=", (unsigned int)profile_id);

  ui_log_raw("Starting net PRX");
  load_net_plugin(profile_id);
  sceKernelExitDeleteThread(0);
  return 0;
}
