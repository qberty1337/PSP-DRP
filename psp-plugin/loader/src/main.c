#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <string.h>

#ifndef LOADER_MODULE_NAME
#define LOADER_MODULE_NAME "PSPDRP_Loader"
#endif

PSP_MODULE_INFO(LOADER_MODULE_NAME, PSP_MODULE_USER, 1, 0);

#ifndef LOADER_LOG_PATH
#define LOADER_LOG_PATH "ms0:/psp_drp.log"
#endif

#ifndef LOG_PREFIX
#define LOG_PREFIX "[LOADER] "
#endif
#define NET_PRX_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp_net.prx"
#define CONFIG_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp.ini"
#define HEX_CHARS "0123456789ABCDEF"

#define RPC_START_MAGIC 0x31504352

static int g_logging_enabled = 0;

#ifndef START_PROFILE_ID
#define START_PROFILE_ID 1
#endif

#ifndef AUTO_START_DELAY_MS
#define AUTO_START_DELAY_MS 500
#endif

#ifndef AUTO_START_MAX_ATTEMPTS
#define AUTO_START_MAX_ATTEMPTS 50
#endif

#ifndef DEFAULT_SKIP_BUTTON
#define DEFAULT_SKIP_BUTTON PSP_CTRL_LTRIGGER
#endif

typedef struct {
  unsigned int magic;
  int profile_id;
  unsigned int flags;
} RpcStartArgs;

#define RPC_START_FLAG_FROM_UI 0x01

#ifndef START_FLAGS
#define DEFAULT_START_FLAGS RPC_START_FLAG_FROM_UI
#else
#define DEFAULT_START_FLAGS START_FLAGS
#endif

static void loader_log_raw(const char *msg) {
  SceUID fd;
  int len = 0;
  int prefix_len = 0;

  if (!g_logging_enabled) {
    return;
  }

  if (msg == NULL) {
    return;
  }

  while (msg[len] != '\0') {
    len++;
  }

  while (LOG_PREFIX[prefix_len] != '\0') {
    prefix_len++;
  }

  fd = sceIoOpen(LOADER_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND,
                 0777);
  if (fd < 0) {
    return;
  }

  if (len > 0) {
    if (prefix_len > 0) {
      sceIoWrite(fd, LOG_PREFIX, prefix_len);
    }
    sceIoWrite(fd, msg, len);
    sceIoWrite(fd, "\n", 1);
  }
  sceIoClose(fd);
}

static void u32_to_hex(char *out, unsigned int value) {
  int i;
  for (i = 0; i < 8; i++) {
    out[7 - i] = HEX_CHARS[value & 0xF];
    value >>= 4;
  }
  out[8] = '\0';
}

static char to_upper_char(char c) {
  if (c >= 'a' && c <= 'z') {
    return (char)(c - 'a' + 'A');
  }
  return c;
}

static int token_equals(const char *a, const char *b) {
  int i = 0;
  if (a == NULL || b == NULL) {
    return 0;
  }
  while (a[i] != '\0' && b[i] != '\0') {
    if (to_upper_char(a[i]) != to_upper_char(b[i])) {
      return 0;
    }
    i++;
  }
  return a[i] == '\0' && b[i] == '\0';
}

static unsigned int parse_skip_button(const char *value) {
  char token[32];
  int len = 0;
  if (value == NULL) {
    return DEFAULT_SKIP_BUTTON;
  }
  while (*value == ' ' || *value == '\t') {
    value++;
  }
  while (*value != '\0' && *value != '\r' && *value != '\n' && *value != ' ' &&
         *value != '\t' && *value != ';' && *value != '#') {
    if (len < (int)(sizeof(token) - 1)) {
      token[len++] = to_upper_char(*value);
    }
    value++;
  }
  token[len] = '\0';

  if (token_equals(token, "L") || token_equals(token, "LTRIGGER")) {
    return PSP_CTRL_LTRIGGER;
  }
  if (token_equals(token, "R") || token_equals(token, "RTRIGGER")) {
    return PSP_CTRL_RTRIGGER;
  }
  if (token_equals(token, "SELECT")) {
    return PSP_CTRL_SELECT;
  }
  if (token_equals(token, "START")) {
    return PSP_CTRL_START;
  }
  if (token_equals(token, "UP")) {
    return PSP_CTRL_UP;
  }
  if (token_equals(token, "DOWN")) {
    return PSP_CTRL_DOWN;
  }
  if (token_equals(token, "LEFT")) {
    return PSP_CTRL_LEFT;
  }
  if (token_equals(token, "RIGHT")) {
    return PSP_CTRL_RIGHT;
  }
  if (token_equals(token, "TRIANGLE")) {
    return PSP_CTRL_TRIANGLE;
  }
  if (token_equals(token, "CIRCLE")) {
    return PSP_CTRL_CIRCLE;
  }
  if (token_equals(token, "CROSS")) {
    return PSP_CTRL_CROSS;
  }
  if (token_equals(token, "SQUARE")) {
    return PSP_CTRL_SQUARE;
  }

  return DEFAULT_SKIP_BUTTON;
}

static unsigned int load_skip_button(void) {
  char buf[2048];
  int len;
  char *line;
  char *next;
  SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return DEFAULT_SKIP_BUTTON;
  }
  len = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (len <= 0) {
    return DEFAULT_SKIP_BUTTON;
  }
  buf[len] = '\0';
  line = buf;
  while (*line != '\0') {
    char *p = line;
    char *eq = NULL;
    next = strchr(line, '\n');
    if (next != NULL) {
      *next = '\0';
      next++;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r') {
      p++;
    }
    if (*p == '#' || *p == ';' || *p == '\0') {
      line = (next != NULL) ? next : (line + strlen(line));
      continue;
    }

    eq = strchr(p, '=');
    if (eq != NULL) {
      char *key_end = eq - 1;
      char *val = eq + 1;
      while (key_end > p && (*key_end == ' ' || *key_end == '\t')) {
        *key_end-- = '\0';
      }
      *eq = '\0';
      while (*val == ' ' || *val == '\t') {
        val++;
      }
      if (token_equals(p, "SKIP_BUTTON")) {
        return parse_skip_button(val);
      }
    }

    line = (next != NULL) ? next : (line + strlen(line));
  }

  return DEFAULT_SKIP_BUTTON;
}

static void load_logging_enabled(void) {
  char buf[2048];
  int len;
  char *line;
  char *next;
  SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    g_logging_enabled = 0;
    return;
  }
  len = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (len <= 0) {
    g_logging_enabled = 0;
    return;
  }
  buf[len] = '\0';
  line = buf;
  while (*line != '\0') {
    char *p = line;
    char *eq = NULL;
    next = strchr(line, '\n');
    if (next != NULL) {
      *next = '\0';
      next++;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r') {
      p++;
    }
    if (*p == '#' || *p == ';' || *p == '\0') {
      line = (next != NULL) ? next : (line + strlen(line));
      continue;
    }

    eq = strchr(p, '=');
    if (eq != NULL) {
      char *key_end = eq - 1;
      char *val = eq + 1;
      while (key_end > p && (*key_end == ' ' || *key_end == '\t')) {
        *key_end-- = '\0';
      }
      *eq = '\0';
      while (*val == ' ' || *val == '\t') {
        val++;
      }
      if (token_equals(p, "ENABLE_LOGGING")) {
        g_logging_enabled = (*val == '1') ? 1 : 0;
        return;
      }
    }

    line = (next != NULL) ? next : (line + strlen(line));
  }

  g_logging_enabled = 0;
}

static int parse_int(const char *val) {
  int result = 0;
  int negative = 0;
  if (val == NULL) {
    return 0;
  }
  while (*val == ' ' || *val == '\t') {
    val++;
  }
  if (*val == '-') {
    negative = 1;
    val++;
  }
  while (*val >= '0' && *val <= '9') {
    result = result * 10 + (*val - '0');
    val++;
  }
  return negative ? -result : result;
}

static int load_startup_delay(void) {
  char buf[2048];
  int len;
  char *line;
  char *next;
  SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return AUTO_START_DELAY_MS;
  }
  len = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (len <= 0) {
    return AUTO_START_DELAY_MS;
  }
  buf[len] = '\0';
  line = buf;
  while (*line != '\0') {
    char *p = line;
    char *eq = NULL;
    next = strchr(line, '\n');
    if (next != NULL) {
      *next = '\0';
      next++;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r') {
      p++;
    }
    if (*p == '#' || *p == ';' || *p == '\0') {
      line = (next != NULL) ? next : (line + strlen(line));
      continue;
    }

    eq = strchr(p, '=');
    if (eq != NULL) {
      char *key_end = eq - 1;
      char *val = eq + 1;
      while (key_end > p && (*key_end == ' ' || *key_end == '\t')) {
        *key_end-- = '\0';
      }
      *eq = '\0';
      while (*val == ' ' || *val == '\t') {
        val++;
      }
      if (token_equals(p, "STARTUP_DELAY_MS")) {
        int delay = parse_int(val);
        if (delay >= 0) {
          return delay;
        }
        return AUTO_START_DELAY_MS;
      }
    }

    line = (next != NULL) ? next : (line + strlen(line));
  }

  return AUTO_START_DELAY_MS;
}

static int load_net_plugin(void) {
  char msg[12];
  char hex[9];
  int i;
  SceUID modid = sceKernelLoadModule(NET_PRX_PATH, 0, NULL);
  if (modid < 0) {
    loader_log_raw("Load net PRX failed");
    u32_to_hex(hex, (unsigned int)modid);
    msg[0] = '0';
    msg[1] = 'x';
    for (i = 0; i < 8; i++) {
      msg[2 + i] = hex[i];
    }
    msg[10] = '\0';
    loader_log_raw(msg);
    return -1;
  }
  {
    RpcStartArgs args;
    int start_res;

    memset(&args, 0, sizeof(args));
    args.magic = RPC_START_MAGIC;
    args.profile_id = START_PROFILE_ID;
    args.flags = DEFAULT_START_FLAGS;

    start_res = sceKernelStartModule(modid, sizeof(args), &args, NULL, NULL);
    if (start_res < 0) {
      char msg[12];
      char hex[9];
      int i;
      loader_log_raw("Start net PRX failed");
      u32_to_hex(hex, (unsigned int)start_res);
      msg[0] = '0';
      msg[1] = 'x';
      for (i = 0; i < 8; i++) {
        msg[2 + i] = hex[i];
      }
      msg[10] = '\0';
      loader_log_raw(msg);
      sceKernelUnloadModule(modid);
      return -1;
    }
  }
  loader_log_raw("Net PRX started");
  return 0;
}

#ifdef AUTO_START_NET
static int file_exists(const char *path) {
  SceIoStat stat;
  if (path == NULL) {
    return 0;
  }
  if (sceIoGetstat(path, &stat) < 0) {
    return 0;
  }
  return 1;
}

static int auto_start_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  SceCtrlData pad;
  int attempts = 0;
  int max_attempts = AUTO_START_MAX_ATTEMPTS;
  int startup_delay = load_startup_delay();

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

  loader_log_raw("Auto-start thread delay");
  sceKernelDelayThread(startup_delay * 1000);

  if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
    unsigned int skip_button = load_skip_button();
    if (skip_button != 0 && (pad.Buttons & skip_button)) {
      loader_log_raw("Auto-start skipped (skip button held)");
      return 0;
    }
  }

  while (attempts < max_attempts) {
    loader_log_raw("Auto-start attempt");
    if (file_exists(NET_PRX_PATH)) {
      if (load_net_plugin() == 0) {
        return 0;
      }
    } else {
      loader_log_raw("Auto-start net PRX missing");
    }
    attempts++;
    sceKernelDelayThread(200 * 1000);
  }

  loader_log_raw("Auto-start net PRX failed");
  return 0;
}
#endif

int module_start(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  load_logging_enabled();

  loader_log_raw("module_start called");

#ifdef AUTO_START_NET
  loader_log_raw("Auto-start net PRX");
  {
    SceUID start_thid =
        sceKernelCreateThread("PSPDRP_AutoStart", auto_start_thread, 0x11,
                              0x2000, PSP_THREAD_ATTR_USER, NULL);
    if (start_thid >= 0) {
      sceKernelStartThread(start_thid, 0, NULL);
    } else {
      loader_log_raw("Auto-start thread create failed");
    }
  }
#endif

  return 0;
}

int module_stop(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  return 0;
}
