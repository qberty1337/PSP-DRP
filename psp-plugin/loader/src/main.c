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
#define USB_PRX_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp_usb.prx"
#define CONFIG_PATH "ms0:/SEPLUGINS/pspdrp/psp_drp.ini"
#define HEX_CHARS "0123456789ABCDEF"

/* Error code for module already loaded */
#define SCE_KERNEL_ERROR_EXCLUSIVE_LOAD ((int)0x80020112)

#define RPC_START_MAGIC 0x31504352

/**
 * Games that are completely incompatible with the plugin.
 * The net PRX will NOT be loaded for these games.
 *
 * NOTE: Due to PSP architecture limitations, game detection only works AFTER
 * the loader thread has started. Some games (like PQ) freeze if ANY thread
 * is created during their boot sequence. For these games, users must manually
 * exclude the plugin from loading via GAME.TXT configuration.
 *
 * This list only helps for games that tolerate thread creation but have
 * other compatibility issues with the net PRX.
 */
static const char *g_incompatible_games[] = {
    "ULUS10046", /* PQ: Practical Intelligence Quotient - freezes on ANY thread
                    creation */
    NULL         /* End marker */
};

static int g_logging_enabled = 0;

/**
 * Simple SFO header structure for game ID detection.
 */
typedef struct {
  unsigned int magic;
  unsigned int version;
  unsigned int key_table_offset;
  unsigned int data_table_offset;
  unsigned int num_entries;
} SfoHeader;

typedef struct {
  unsigned short key_offset;
  unsigned short data_format;
  unsigned int data_len;
  unsigned int data_max_len;
  unsigned int data_offset;
} SfoEntry;

/**
 * Get current game ID from PARAM.SFO.
 * Returns 0 on success, -1 on failure.
 */
static int get_current_game_id(char *game_id_out, int max_len) {
  /* Try UMD first, then memory stick paths */
  static const char *sfo_paths[] = {
      "disc0:/PSP_GAME/PARAM.SFO",
      "ms0:/PSP/GAME/__SCE__*/PARAM.SFO", /* Can't use wildcard, try specific */
      NULL};

  SceUID fd;
  SfoHeader header;
  SfoEntry entry;
  char key_buf[32];
  int i;
  int found = 0;

  (void)sfo_paths; /* Unused for now */

  /* Try disc0 first (UMD games) */
  fd = sceIoOpen("disc0:/PSP_GAME/PARAM.SFO", PSP_O_RDONLY, 0);
  if (fd < 0) {
    /* Try EBOOT path for digital games - use sceIoGetstat to find */
    return -1; /* Can't detect game */
  }

  /* Read SFO header */
  if (sceIoRead(fd, &header, sizeof(header)) != sizeof(header)) {
    sceIoClose(fd);
    return -1;
  }

  /* Check magic (0x46535000 = "PSF\0") */
  if (header.magic != 0x46535000) {
    sceIoClose(fd);
    return -1;
  }

  /* Search for DISC_ID entry */
  for (i = 0; i < (int)header.num_entries && !found; i++) {
    sceIoLseek(fd, 20 + i * 16, PSP_SEEK_SET);
    if (sceIoRead(fd, &entry, sizeof(entry)) != sizeof(entry)) {
      break;
    }

    /* Read key name */
    sceIoLseek(fd, header.key_table_offset + entry.key_offset, PSP_SEEK_SET);
    if (sceIoRead(fd, key_buf, sizeof(key_buf) - 1) <= 0) {
      break;
    }
    key_buf[sizeof(key_buf) - 1] = '\0';

    /* Check if this is DISC_ID */
    if (key_buf[0] == 'D' && key_buf[1] == 'I' && key_buf[2] == 'S' &&
        key_buf[3] == 'C' && key_buf[4] == '_' && key_buf[5] == 'I' &&
        key_buf[6] == 'D' && key_buf[7] == '\0') {
      /* Read the value */
      sceIoLseek(fd, header.data_table_offset + entry.data_offset,
                 PSP_SEEK_SET);
      int read_len = (entry.data_len < (unsigned int)max_len)
                         ? (int)entry.data_len
                         : max_len - 1;
      if (sceIoRead(fd, game_id_out, read_len) > 0) {
        game_id_out[read_len] = '\0';
        found = 1;
      }
    }
  }

  sceIoClose(fd);
  return found ? 0 : -1;
}

/**
 * Get current game ID and title from PARAM.SFO.
 * Returns 0 on success, -1 on failure.
 */
static int get_current_game_info(char *game_id_out, int id_max_len,
                                 char *title_out, int title_max_len) {
  SceUID fd;
  SfoHeader header;
  SfoEntry entry;
  char key_buf[32];
  int i;
  int found_id = 0;
  int found_title = 0;

  /* Initialize outputs */
  if (game_id_out && id_max_len > 0)
    game_id_out[0] = '\0';
  if (title_out && title_max_len > 0)
    title_out[0] = '\0';

  /* Try disc0 first (UMD games) */
  fd = sceIoOpen("disc0:/PSP_GAME/PARAM.SFO", PSP_O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  /* Read SFO header */
  if (sceIoRead(fd, &header, sizeof(header)) != sizeof(header)) {
    sceIoClose(fd);
    return -1;
  }

  /* Check magic (0x46535000 = "PSF\0") */
  if (header.magic != 0x46535000) {
    sceIoClose(fd);
    return -1;
  }

  /* Search for DISC_ID and TITLE entries */
  for (i = 0; i < (int)header.num_entries && !(found_id && found_title); i++) {
    sceIoLseek(fd, 20 + i * 16, PSP_SEEK_SET);
    if (sceIoRead(fd, &entry, sizeof(entry)) != sizeof(entry)) {
      break;
    }

    /* Read key name */
    sceIoLseek(fd, header.key_table_offset + entry.key_offset, PSP_SEEK_SET);
    if (sceIoRead(fd, key_buf, sizeof(key_buf) - 1) <= 0) {
      break;
    }
    key_buf[sizeof(key_buf) - 1] = '\0';

    /* Check if this is DISC_ID */
    if (!found_id && game_id_out && key_buf[0] == 'D' && key_buf[1] == 'I' &&
        key_buf[2] == 'S' && key_buf[3] == 'C' && key_buf[4] == '_' &&
        key_buf[5] == 'I' && key_buf[6] == 'D' && key_buf[7] == '\0') {
      sceIoLseek(fd, header.data_table_offset + entry.data_offset,
                 PSP_SEEK_SET);
      int read_len = (entry.data_len < (unsigned int)id_max_len)
                         ? (int)entry.data_len
                         : id_max_len - 1;
      if (sceIoRead(fd, game_id_out, read_len) > 0) {
        game_id_out[read_len] = '\0';
        found_id = 1;
      }
    }

    /* Check if this is TITLE */
    if (!found_title && title_out && key_buf[0] == 'T' && key_buf[1] == 'I' &&
        key_buf[2] == 'T' && key_buf[3] == 'L' && key_buf[4] == 'E' &&
        key_buf[5] == '\0') {
      sceIoLseek(fd, header.data_table_offset + entry.data_offset,
                 PSP_SEEK_SET);
      int read_len = (entry.data_len < (unsigned int)title_max_len)
                         ? (int)entry.data_len
                         : title_max_len - 1;
      if (sceIoRead(fd, title_out, read_len) > 0) {
        title_out[read_len] = '\0';
        found_title = 1;
      }
    }
  }

  sceIoClose(fd);
  return found_id ? 0 : -1;
}

/**
 * Check if a game ID is in the incompatible list.
 */
static int is_game_incompatible(const char *game_id) {
  int i;
  int j;
  if (game_id == NULL || game_id[0] == '\0') {
    return 0;
  }
  for (i = 0; g_incompatible_games[i] != NULL; i++) {
    /* Simple string compare */
    const char *a = game_id;
    const char *b = g_incompatible_games[i];
    j = 0;
    while (a[j] != '\0' && b[j] != '\0' && a[j] == b[j]) {
      j++;
    }
    if (a[j] == '\0' && b[j] == '\0') {
      return 1; /* Match */
    }
  }
  return 0;
}

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
  char game_id[16];
  char game_title[64];
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

static int load_usb_mode(void) {
  char buf[2048];
  int len;
  char *line;
  char *next;
  SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return 0; /* Default: USB mode disabled */
  }
  len = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (len <= 0) {
    return 0;
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
      if (token_equals(p, "USB_MODE")) {
        return (*val == '1') ? 1 : 0;
      }
    }

    line = (next != NULL) ? next : (line + strlen(line));
  }

  return 0; /* Default: USB mode disabled */
}

/* USB Module startup args magic - must match USB module's USB_STARTUP_MAGIC */
#define USB_STARTUP_MAGIC 0x55534247 /* "USBG" */

typedef struct {
  unsigned int magic;
  char game_id[16];
  char game_title[64];
} UsbStartupArgs;

static int load_usb_plugin(const char *game_id, const char *game_title) {
  char msg[12];
  char hex[9];
  int i;
  SceUID modid = sceKernelLoadModule(USB_PRX_PATH, 0, NULL);
  if (modid < 0) {
    /* Check if module is already loaded */
    if (modid == SCE_KERNEL_ERROR_EXCLUSIVE_LOAD) {
      loader_log_raw("USB PRX already loaded");
      return 0; /* Success - module is already running */
    }
    loader_log_raw("Load USB PRX failed");
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
    int start_res;
    UsbStartupArgs startup_args;

    /* Prepare startup args with game ID and title */
    memset(&startup_args, 0, sizeof(startup_args));
    startup_args.magic = USB_STARTUP_MAGIC;
    if (game_id != NULL && game_id[0] != '\0') {
      int j;
      for (j = 0; j < 15 && game_id[j] != '\0'; j++) {
        startup_args.game_id[j] = game_id[j];
      }
      startup_args.game_id[j] = '\0';
      loader_log_raw("Passing game ID to USB PRX");
    }
    if (game_title != NULL && game_title[0] != '\0') {
      int j;
      for (j = 0; j < 63 && game_title[j] != '\0'; j++) {
        startup_args.game_title[j] = game_title[j];
      }
      startup_args.game_title[j] = '\0';
      loader_log_raw("Passing game title to USB PRX");
    }

    start_res = sceKernelStartModule(modid, sizeof(startup_args), &startup_args,
                                     NULL, NULL);
    if (start_res < 0) {
      char msg[12];
      char hex[9];
      int i;
      loader_log_raw("Start USB PRX failed");
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
  loader_log_raw("USB PRX started");
  return 0;
}

static int load_net_plugin(const char *game_id, const char *game_title) {
  char msg[12];
  char hex[9];
  int i;
  SceUID modid = sceKernelLoadModule(NET_PRX_PATH, 0, NULL);
  if (modid < 0) {
    /* Check if module is already loaded */
    if (modid == SCE_KERNEL_ERROR_EXCLUSIVE_LOAD) {
      loader_log_raw("Net PRX already loaded");
      return 0; /* Success - module is already running */
    }
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

    /* Pass game info to net plugin */
    if (game_id != NULL && game_id[0] != '\0') {
      int j;
      for (j = 0; j < 15 && game_id[j] != '\0'; j++) {
        args.game_id[j] = game_id[j];
      }
      args.game_id[j] = '\0';
      loader_log_raw("Passing game ID to NET PRX");
    }
    if (game_title != NULL && game_title[0] != '\0') {
      int j;
      for (j = 0; j < 63 && game_title[j] != '\0'; j++) {
        args.game_title[j] = game_title[j];
      }
      args.game_title[j] = '\0';
      loader_log_raw("Passing game title to NET PRX");
    }

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
  char game_id[16] = {0};
  char game_title[64] = {0};

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

  loader_log_raw("Auto-start thread delay");
  sceKernelDelayThread(startup_delay * 1000);

  /* Check for incompatible games BEFORE loading net PRX */
  /* Also get game title for USB mode */
  if (get_current_game_info(game_id, sizeof(game_id), game_title,
                            sizeof(game_title)) == 0) {
    loader_log_raw("Detected game:");
    loader_log_raw(game_id);
    if (game_title[0] != '\0') {
      loader_log_raw(game_title);
    }
    if (is_game_incompatible(game_id)) {
      loader_log_raw("Game incompatible, skipping net PRX");
      return 0;
    }
  }

  {
    unsigned int skip_button = load_skip_button();
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
      if (skip_button != 0 && (pad.Buttons & skip_button)) {
        loader_log_raw("Auto-start skipped (skip button held)");
        loader_log_raw("Waiting for SELECT+skip to reactivate...");

        /* Wait for reactivation combo: SELECT + skip button */
        while (1) {
          sceKernelDelayThread(100 * 1000); /* 100ms polling */
          if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
            if ((pad.Buttons & PSP_CTRL_SELECT) &&
                (pad.Buttons & skip_button)) {
              loader_log_raw("Reactivation combo detected!");
              break; /* Exit wait loop and proceed to load */
            }
          }
        }
      }
    }
  }
  /* Check USB mode setting */
  int usb_mode = load_usb_mode();

  while (attempts < max_attempts) {
    loader_log_raw("Auto-start attempt");

    if (usb_mode) {
      /* USB mode - load USB kernel PRX */
      if (file_exists(USB_PRX_PATH)) {
        loader_log_raw("Loading USB PRX");
        if (load_usb_plugin(game_id, game_title) == 0) {
          return 0;
        }
      } else {
        loader_log_raw("USB PRX missing");
      }
    } else {
      /* Network mode - load NET user PRX */
      if (file_exists(NET_PRX_PATH)) {
        loader_log_raw("Loading NET PRX");
        if (load_net_plugin(game_id, game_title) == 0) {
          return 0;
        }
      } else {
        loader_log_raw("NET PRX missing");
      }
    }

    attempts++;
    sceKernelDelayThread(200 * 1000);
  }

  loader_log_raw(usb_mode ? "Auto-start USB PRX failed"
                          : "Auto-start NET PRX failed");
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
