/**
 * Game Detection Module
 */

#ifndef GAME_DETECT_H
#define GAME_DETECT_H

#include "discord_rpc.h"

/**
 * Initialize game detection
 */
void game_detect_init(void);


/**
 * Detect the currently running game/application
 * 
 * @param info Output structure for game information
 * @return 0 on success, negative on error
 */
int game_detect_current(GameInfo *info);

/**
 * Extract icon data for the current game
 * 
 * @param game_id Game ID to get icon for
 * @param buffer Output buffer for icon data
 * @param buffer_size Size of output buffer
 * @param icon_size Output: actual icon size
 * @return 0 on success, negative on error
 */
int game_detect_get_icon(const char *game_id, uint8_t *buffer, 
                         uint32_t buffer_size, uint32_t *icon_size);

#endif /* GAME_DETECT_H */
