#define AUTO_START_NET 1
#define AUTO_START_NET 1
#define LOADER_MODULE_NAME "PSPDRP_Loader_Game"
#define LOADER_LOG_PATH "ms0:/psp_drp.log"
#define LOG_PREFIX "[LOADER_GAME] "
#define START_FLAGS 0

// Wait 3 seconds before activating the plugin to give games
// with network loading enough time to fully initialize
#define AUTO_START_DELAY_MS 3000

#include "../../loader/src/main.c"
