/**
 * Game Detection Implementation
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <pspumd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_detect.h"
#include "network.h"
#include "sfo.h"

/* Path to running game's directory */
static char g_game_path[256] = {0};

/* Start time of current game */
static uint32_t g_game_start_time = 0;

/* Forward declarations */
static int detect_umd_game(GameInfo *info);
static int detect_iso_game(GameInfo *info);
static int detect_eboot_game(GameInfo *info);
static uint8_t determine_state(const char *game_id);
static int detect_module_game(GameInfo *info);
static uint32_t get_unix_time(void);
static void copy_str(char *dst, size_t dst_size, const char *src);
static int build_path(char *out, size_t out_size, const char *base,
                      const char *name, const char *suffix);

/**
 * Initialize game detection
 */
void game_detect_init(void) {
  memset(g_game_path, 0, sizeof(g_game_path));
  g_game_start_time = 0;
}

/**
 * Detect the currently running game/application
 */
int game_detect_current(GameInfo *info) {
  int result;

  memset(info, 0, sizeof(GameInfo));

  /* Try UMD first */
  result = detect_umd_game(info);
  if (result == 0 && info->game_id[0] != '\0') {
    goto found;
  }

  /* Try mounted ISO */
  result = detect_iso_game(info);
  if (result == 0 && info->game_id[0] != '\0') {
    goto found;
  }

  /* Try EBOOT/PSN/Homebrew game */
  result = detect_eboot_game(info);
  if (result == 0 && info->game_id[0] != '\0') {
    goto found;
  }

  /* Try detecting from loaded modules */
  result = detect_module_game(info);
  if (result == 0 && info->game_id[0] != '\0') {
    goto found;
  }

  /* Nothing found - unknown state */
  copy_str(info->game_id, sizeof(info->game_id), "UNKNOWN");
  copy_str(info->title, sizeof(info->title), "Unknown Game");
  info->state = STATE_GAME;
  info->start_time = 0;
  info->has_icon = 0;

  return 0;

found:
  /* Set state based on game ID pattern */
  info->state = determine_state(info->game_id);

  /* Track game start time */
  if (g_game_start_time == 0) {
    g_game_start_time = get_unix_time();
  }
  info->start_time = g_game_start_time;

  return 0;
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
 * Build file path: base/name/suffix or base/suffix if name is empty
 */
static int build_path(char *out, size_t out_size, const char *base,
                      const char *name, const char *suffix) {
  size_t base_len;
  size_t name_len;
  size_t suffix_len;
  char *p;
  int has_name;
  int base_has_slash;
  int suffix_has_slash;

  if (out == NULL || base == NULL || suffix == NULL) {
    return -1;
  }

  base_len = strlen(base);
  name_len = (name != NULL) ? strlen(name) : 0;
  suffix_len = strlen(suffix);
  has_name = (name != NULL && name[0] != '\0');

  /* Check for existing slashes to avoid doubles */
  base_has_slash = (base_len > 0 && base[base_len - 1] == '/');
  suffix_has_slash = (suffix_len > 0 && suffix[0] == '/');

  p = out;

  /* Copy base, but skip trailing slash if present */
  if (base_has_slash && base_len > 0) {
    memcpy(p, base, base_len - 1);
    p += base_len - 1;
  } else {
    memcpy(p, base, base_len);
    p += base_len;
  }

  /* Add name with surrounding slashes */
  if (has_name) {
    *p++ = '/';
    memcpy(p, name, name_len);
    p += name_len;
  }

  /* Add suffix, handling leading slash */
  if (suffix_len > 0) {
    if (!suffix_has_slash && (has_name || base_len > 0)) {
      *p++ = '/';
    }
    memcpy(p, suffix, suffix_len);
    p += suffix_len;
  }

  *p = '\0';
  return 0;
}

/**
 * Detect UMD disc game
 */
static int detect_umd_game(GameInfo *info) {
  SceUID fd;
  SfoData sfo;
  char path[64] = "disc0:/PSP_GAME/PARAM.SFO";

  /* Check if UMD is present */
  if (sceUmdCheckMedium() == 0) {
    return -1;
  }

  /* Try to read PARAM.SFO from disc */
  if (sfo_parse_file(path, &sfo) < 0) {
    return -1;
  }

  copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
  copy_str(info->title, sizeof(info->title), sfo.title);

  /* Check for icon */
  fd = sceIoOpen("disc0:/PSP_GAME/ICON0.PNG", PSP_O_RDONLY, 0);
  if (fd >= 0) {
    info->has_icon = 1;
    sceIoClose(fd);
    copy_str(g_game_path, sizeof(g_game_path), "disc0:/PSP_GAME");
  }

  return 0;
}

/**
 * Detect mounted ISO/CSO game
 */
static int detect_iso_game(GameInfo *info) {
  SfoData sfo;
  SceUID fd;

  /* Check if ISO is mounted (usually at disc0: like UMD via ARK/Inferno) */
  /* Try alternate mount points used by some plugins */
  const char *iso_mounts[] = {"host0:/PSP_GAME/PARAM.SFO",
                              "umd0:/PSP_GAME/PARAM.SFO", NULL};

  for (int i = 0; iso_mounts[i] != NULL; i++) {
    if (sfo_parse_file(iso_mounts[i], &sfo) == 0) {
      copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
      copy_str(info->title, sizeof(info->title), sfo.title);

      /* Extract base path for icon */
      char icon_path[256];
      copy_str(icon_path, sizeof(icon_path), iso_mounts[i]);
      char *param_sfo = strstr(icon_path, "PARAM.SFO");
      if (param_sfo) {
        strcpy(param_sfo, "ICON0.PNG");
        fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
        if (fd >= 0) {
          info->has_icon = 1;
          sceIoClose(fd);
          /* Store the base game path */
          *param_sfo = '\0';
          if (param_sfo > icon_path && *(param_sfo - 1) == '/') {
            *(param_sfo - 1) = '\0';
          }
          copy_str(g_game_path, sizeof(g_game_path), icon_path);
        }
      }

      return 0;
    }
  }

  return -1;
}

/**
 * Detect EBOOT/PSN/Homebrew game running from memory stick
 * These games run from ms0:/PSP/GAME/ or ef0:/PSP/GAME/ and some CFWs
 * expose the running game via game0:/ virtual device
 */
static int detect_eboot_game(GameInfo *info) {
  SfoData sfo;
  SceUID fd;

  /* Paths where PSN/Homebrew games may expose PARAM.SFO */
  const char *eboot_mounts[] = {
      "disc0:/PSP_GAME/PARAM.SFO", /* ARK/Inferno mounts ISOs and EBOOTs here */
      "game0:/PARAM.SFO",          /* CFW virtual device for running game */
      "game0:/PSP_GAME/PARAM.SFO", /* Alternative layout */
      "ef0:/PSP_GAME/PARAM.SFO",   /* PSP Go internal storage */
      "ms0:/PSP/GAME/__SCE__/PARAM.SFO", /* Some homebrews */
      NULL};

  net_log("detect_eboot: checking paths");

  for (int i = 0; eboot_mounts[i] != NULL; i++) {
    net_log("detect_eboot: trying %s", eboot_mounts[i]);
    if (sfo_parse_file(eboot_mounts[i], &sfo) == 0) {
      net_log("detect_eboot: found! id=%s title=%s", sfo.disc_id, sfo.title);
      copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
      copy_str(info->title, sizeof(info->title), sfo.title);

      /* Extract base path for icon */
      char icon_path[256];
      copy_str(icon_path, sizeof(icon_path), eboot_mounts[i]);
      char *param_sfo = strstr(icon_path, "PARAM.SFO");
      if (param_sfo) {
        strcpy(param_sfo, "ICON0.PNG");
        fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
        if (fd >= 0) {
          info->has_icon = 1;
          sceIoClose(fd);
          /* Store the base game path */
          *param_sfo = '\0';
          if (param_sfo > icon_path && *(param_sfo - 1) == '/') {
            *(param_sfo - 1) = '\0';
          }
          copy_str(g_game_path, sizeof(g_game_path), icon_path);
        }
      }

      return 0;
    }
  }

  net_log("detect_eboot: no paths worked");
  return -1;
}

/**
 * Detect game by examining loaded modules
 * This finds homebrew games by looking for modules loaded from ms0:/PSP/GAME/
 */
static int detect_module_game(GameInfo *info) {
  SceUID mod_ids[64];
  int num_modules = 0;
  int ret;
  int i;
  SceKernelModuleInfo mod_info;
  SfoData sfo;
  char sfo_path[256];
  int j;
  int has_underscore;

  net_log("detect_module: enumerating modules");

  /* Get list of loaded modules - ret is error code, num_modules is output count
   */
  ret = sceKernelGetModuleIdList(mod_ids, sizeof(mod_ids), &num_modules);
  if (ret < 0) {
    net_log("detect_module: GetModuleIdList ret=%d", ret);
  }

  net_log("detect_module: found %d modules", num_modules);

  /* First pass: find the likely game module (no underscore, not sce*, not
   * PSPDRP*) */
  for (i = 0; i < num_modules && i < 64; i++) {
    memset(&mod_info, 0, sizeof(mod_info));
    mod_info.size = sizeof(mod_info);

    if (sceKernelQueryModuleInfo(mod_ids[i], &mod_info) < 0) {
      continue;
    }

    if (mod_info.name[0] == '\0') {
      continue;
    }

    net_log("detect_module: mod=%s", mod_info.name);

    /* Skip system modules starting with 'sce' */
    if (strncmp(mod_info.name, "sce", 3) == 0) {
      continue;
    }

    /* Skip our own plugin modules */
    if (strncmp(mod_info.name, "PSPDRP", 6) == 0) {
      continue;
    }

    /* Check for underscore - modules with underscores are usually plugins */
    has_underscore = 0;
    for (j = 0; mod_info.name[j] != '\0'; j++) {
      if (mod_info.name[j] == '_') {
        has_underscore = 1;
        break;
      }
    }

    if (has_underscore) {
      continue;
    }

    /* This module looks like a game! Use its name directly as the title */
    net_log("detect_module: identified game module: %s", mod_info.name);

    /* Try to find PARAM.SFO in ms0:/PSP/GAME/<modname>/ */
    build_path(sfo_path, sizeof(sfo_path), "ms0:/PSP/GAME/", mod_info.name,
               "/PARAM.SFO");
    net_log("detect_module: trying %s", sfo_path);

    if (sfo_parse_file(sfo_path, &sfo) == 0) {
      net_log("detect_module: found! id=%s title=%s", sfo.disc_id, sfo.title);
      copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
      copy_str(info->title, sizeof(info->title), sfo.title);

      /* Build icon path and check */
      build_path(g_game_path, sizeof(g_game_path), "ms0:/PSP/GAME/",
                 mod_info.name, "");
      char icon_path[256];
      build_path(icon_path, sizeof(icon_path), g_game_path, "/ICON0.PNG", "");
      SceUID fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
      if (fd >= 0) {
        info->has_icon = 1;
        sceIoClose(fd);
      }

      return 0;
    }

    /* Try ef0 (PSP Go internal) */
    build_path(sfo_path, sizeof(sfo_path), "ef0:/PSP/GAME/", mod_info.name,
               "/PARAM.SFO");
    if (sfo_parse_file(sfo_path, &sfo) == 0) {
      net_log("detect_module: found on ef0! id=%s title=%s", sfo.disc_id,
              sfo.title);
      copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
      copy_str(info->title, sizeof(info->title), sfo.title);

      build_path(g_game_path, sizeof(g_game_path), "ef0:/PSP/GAME/",
                 mod_info.name, "");
      char icon_path[256];
      build_path(icon_path, sizeof(icon_path), g_game_path, "/ICON0.PNG", "");
      SceUID fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
      if (fd >= 0) {
        info->has_icon = 1;
        sceIoClose(fd);
      }

      return 0;
    }

    /* No PARAM.SFO found, but we identified a game module - use module name as
     * title */
    net_log("detect_module: using module name as title: %s", mod_info.name);
    copy_str(info->game_id, sizeof(info->game_id), mod_info.name);
    copy_str(info->title, sizeof(info->title), mod_info.name);
    info->has_icon = 0;
    return 0;
  }

  /* Fallback: scan ms0:/PSP/GAME/ directory for any game with valid PARAM.SFO
   */
  /* This is a last resort when module enumeration fails */
  net_log("detect_module: trying directory scan");

  {
    SceUID dir = sceIoDopen("ms0:/PSP/GAME");
    if (dir >= 0) {
      SceIoDirent entry;
      int found_count = 0;
      memset(&entry, 0, sizeof(entry));

      while (sceIoDread(dir, &entry) > 0) {
        /* Skip . and .. and special folders */
        if (entry.d_name[0] == '.' || entry.d_name[0] == '_') {
          memset(&entry, 0, sizeof(entry));
          continue;
        }

        /* Only check directories */
        if (!FIO_S_ISDIR(entry.d_stat.st_mode)) {
          memset(&entry, 0, sizeof(entry));
          continue;
        }

        net_log("detect_module: scan dir=%s", entry.d_name);

        /* Try to read PARAM.SFO from this folder */
        build_path(sfo_path, sizeof(sfo_path), "ms0:/PSP/GAME/", entry.d_name,
                   "/PARAM.SFO");

        if (sfo_parse_file(sfo_path, &sfo) == 0) {
          /* Found a valid PARAM.SFO - use TITLE even if disc_id is empty */
          found_count++;
          net_log("detect_module: scan found id='%s' title='%s'", sfo.disc_id,
                  sfo.title);

          /* Use this game if it has a valid title */
          if (sfo.title[0] != '\0') {
            /* Use folder name as ID if disc_id is empty */
            if (sfo.disc_id[0] != '\0') {
              copy_str(info->game_id, sizeof(info->game_id), sfo.disc_id);
            } else {
              copy_str(info->game_id, sizeof(info->game_id), entry.d_name);
            }
            copy_str(info->title, sizeof(info->title), sfo.title);

            build_path(g_game_path, sizeof(g_game_path), "ms0:/PSP/GAME/",
                       entry.d_name, "");
            char icon_path[256];
            build_path(icon_path, sizeof(icon_path), g_game_path, "/ICON0.PNG",
                       "");
            SceUID fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
            if (fd >= 0) {
              info->has_icon = 1;
              sceIoClose(fd);
            }

            sceIoDclose(dir);
            return 0;
          }
        }

        memset(&entry, 0, sizeof(entry));
      }
      sceIoDclose(dir);
    } else {
      net_log("detect_module: failed to open ms0:/PSP/GAME dir=%d", dir);
    }
  }

  net_log("detect_module: no matching module found");
  return -1;
}

/**
 * Determine the state based on game ID pattern
 */
static uint8_t determine_state(const char *game_id) {
  if (game_id == NULL || game_id[0] == '\0') {
    return STATE_XMB;
  }

  /* Standard game IDs start with region codes */
  if (strncmp(game_id, "UC", 2) == 0 || /* UCUS, UCKS, etc. */
      strncmp(game_id, "UL", 2) == 0 || /* ULUS, ULJM, etc. */
      strncmp(game_id, "NP", 2) == 0 || /* NPUH, NPHG, etc. */
      strncmp(game_id, "SC", 2) == 0 || /* SCUS */
      strncmp(game_id, "SL", 2) == 0) { /* SLUS, SLJM, etc. */
    return STATE_GAME;
  }

  /* Homebrew typically has custom IDs */
  if (strncmp(game_id, "HOMEBREW", 8) == 0 || strncmp(game_id, "HB", 2) == 0) {
    return STATE_HOMEBREW;
  }

  return STATE_GAME; /* Default to game */
}

/**
 * Get current Unix timestamp
 * Note: PSP doesn't have native Unix time, so we approximate
 */
static uint32_t get_unix_time(void) {
  ScePspDateTime time;
  if (sceRtcGetCurrentClockLocalTime(&time) < 0) {
    return 0;
  }

  /* Convert to Unix timestamp (simplified, doesn't handle all edge cases) */
  uint32_t days = 0;

  /* Years since 1970 */
  for (int y = 1970; y < (int)time.year; y++) {
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }

  /* Days in current year */
  static const int days_in_month[] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
  int is_leap =
      (time.year % 4 == 0 && (time.year % 100 != 0 || time.year % 400 == 0));

  for (int m = 1; m < (int)time.month; m++) {
    days += days_in_month[m - 1];
    if (m == 2 && is_leap) {
      days += 1;
    }
  }
  days += time.day - 1;

  return days * 86400 + time.hour * 3600 + time.minute * 60 + time.second;
}

/**
 * Extract icon data for the current game
 */
int game_detect_get_icon(const char *game_id, uint8_t *buffer,
                         uint32_t buffer_size, uint32_t *icon_size) {
  char icon_path[256];
  SceUID fd;
  SceIoStat stat;

  (void)game_id; /* Use stored path instead */

  if (g_game_path[0] == '\0') {
    return -1;
  }

  if (build_path(icon_path, sizeof(icon_path), g_game_path, NULL, "ICON0.PNG") <
      0) {
    return -1;
  }

  /* Get file size */
  if (sceIoGetstat(icon_path, &stat) < 0) {
    return -1;
  }

  if (stat.st_size > buffer_size) {
    *icon_size = stat.st_size;
    return -2; /* Buffer too small */
  }

  /* Read icon */
  fd = sceIoOpen(icon_path, PSP_O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  *icon_size = sceIoRead(fd, buffer, stat.st_size);
  sceIoClose(fd);

  return (*icon_size > 0) ? 0 : -1;
}
