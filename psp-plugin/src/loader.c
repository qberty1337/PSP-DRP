#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <stdarg.h>
#include <string.h>

PSP_MODULE_INFO("PSPDRP",
                PSP_MODULE_USER | PSP_MODULE_SINGLE_LOAD |
                    PSP_MODULE_SINGLE_START,
                1, 0);

#define LOADER_LOG_PATH "ms0:/psp_drp_loader.log"
#define NET_PRX_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp_net.prx"

static SceUID g_thread = -1;
static int g_running = 1;
static unsigned int g_prev_buttons = 0;
static SceUID g_net_modid = -1;

static void loader_log(const char *fmt, ...) {
  char buf[128];
  va_list args;
  SceUID fd;
  int len;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  fd = sceIoOpen(LOADER_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND,
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

static int show_wifi_selector(void) {
  pspUtilityNetconfData data;
  int done = 0;

  memset(&data, 0, sizeof(data));
  data.base.size = sizeof(data);
  sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                              &data.base.language);
  data.base.buttonSwap = 0;
  data.base.graphicsThread = 0x11;
  data.base.accessThread = 0x13;
  data.base.fontThread = 0x12;
  data.base.soundThread = 0x10;
  data.action = PSP_NETCONF_ACTION_CONNECTAP;

  if (sceUtilityNetconfInitStart(&data) < 0) {
    return -1;
  }

  while (!done) {
    int status = sceUtilityNetconfGetStatus();
    switch (status) {
    case PSP_UTILITY_DIALOG_VISIBLE:
      sceUtilityNetconfUpdate(1);
      break;
    case PSP_UTILITY_DIALOG_FINISHED:
      sceUtilityNetconfShutdownStart();
      break;
    case PSP_UTILITY_DIALOG_NONE:
      done = 1;
      break;
    default:
      break;
    }
    sceDisplayWaitVblankStart();
  }

  return (data.base.result == 0) ? 0 : -1;
}

static int load_net_plugin(void) {
  if (g_net_modid >= 0) {
    return 0;
  }

  g_net_modid = sceKernelLoadModule(NET_PRX_PATH, 0, NULL);
  if (g_net_modid < 0) {
    loader_log("Load net PRX failed: 0x%08X", (unsigned int)g_net_modid);
    g_net_modid = -1;
    return -1;
  }

  if (sceKernelStartModule(g_net_modid, 0, NULL, NULL, NULL) < 0) {
    loader_log("Start net PRX failed");
    sceKernelUnloadModule(g_net_modid);
    g_net_modid = -1;
    return -1;
  }

  loader_log("Net PRX loaded");
  return 0;
}

static void unload_net_plugin(void) {
  if (g_net_modid >= 0) {
    sceKernelStopModule(g_net_modid, 0, NULL, NULL, NULL);
    sceKernelUnloadModule(g_net_modid);
    g_net_modid = -1;
    loader_log("Net PRX unloaded");
  }
}

static int loader_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  SceCtrlData pad;
  const unsigned int toggle_combo = PSP_CTRL_LTRIGGER | PSP_CTRL_SELECT;

  sceKernelDelayThread(3 * 1000 * 1000);
  loader_log("Loader started");

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

  while (g_running) {
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
      if ((pad.Buttons & toggle_combo) == toggle_combo &&
          (g_prev_buttons & toggle_combo) != toggle_combo) {
        loader_log("Hotkey pressed");

        if (g_net_modid >= 0) {
          unload_net_plugin();
        } else {
          if (show_wifi_selector() == 0) {
            loader_log("WiFi selector OK");
            load_net_plugin();
          } else {
            loader_log("WiFi selector canceled");
          }
        }
      }
      g_prev_buttons = pad.Buttons;
    }

    sceKernelDelayThread(100 * 1000);
  }

  return 0;
}

int module_start(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  loader_log("module_start called");

  g_thread = sceKernelCreateThread("PSPDRP_Loader", loader_thread, 0x11, 0x2000,
                                   PSP_THREAD_ATTR_USER, NULL);

  if (g_thread >= 0) {
    sceKernelStartThread(g_thread, 0, NULL);
  } else {
    loader_log("Thread create failed: %d", g_thread);
  }

  return 0;
}

int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  g_running = 0;
  if (g_thread >= 0) {
    sceKernelWaitThreadEnd(g_thread, NULL);
    sceKernelDeleteThread(g_thread);
  }

  unload_net_plugin();
  return 0;
}
