/**
 * Network Communication Module
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "discord_rpc.h"
#include "config.h"

/**
 * Initialize network subsystem
 * 
 * @return 0 on success, negative on error
 */
int network_init(void);

/**
 * Shutdown network subsystem
 */
void network_shutdown(void);

/**
 * Connect to desktop companion app
 * 
 * @param config Plugin configuration
 * @return 0 on success, 1 if waiting for discovery, negative on error
 */
int network_connect(const PluginConfig *config);

/**
 * Disconnect from desktop app
 */
void network_disconnect(void);

/**
 * Send heartbeat packet
 * 
 * @return 0 on success, negative on error
 */
int network_send_heartbeat(void);

/**
 * Send game info update
 * 
 * @param info Game information to send
 * @return 0 on success, negative on error
 */
int network_send_game_info(const GameInfo *info);

/**
 * Send icon data (chunked)
 * 
 * @param game_id Game ID this icon belongs to
 * @param icon_data PNG icon data
 * @param icon_size Size of icon data
 * @return 0 on success, negative on error
 */
int network_send_icon(const char *game_id, const uint8_t *icon_data, 
                      uint32_t icon_size);

/**
 * Show WiFi profile selector UI and connect
 * 
 * @return 0 on success, negative on error or cancel
 */
int network_show_profile_selector(void);

/**
 * Check for and handle discovery requests
 * 
 * @param config Plugin configuration (updated on success)
 * @return 1 if desktop discovered, 0 if no data, negative on error
 */
int network_handle_discovery(PluginConfig *config);

#endif /* NETWORK_H */
