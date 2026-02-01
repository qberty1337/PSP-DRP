/**
 * Network Communication Module
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "config.h"
#include "discord_rpc.h"

/**
 * Initialize network subsystem
 *
 * @return 0 on success, negative on error
 */
int network_init(void);

/**
 * Set the WLAN profile ID to use when connecting.
 *
 * @param profile_id Profile index (1-16)
 */
void network_set_profile_id(int profile_id);

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

void net_log(const char *fmt, ...);

int network_poll_ack(void);

/**
 * Poll for incoming messages (ACK or icon request)
 *
 * @param game_id_out Buffer to receive requested game ID (10 bytes), can be
 * NULL
 * @return 1 if ACK received, 2 if icon request received, 0 if nothing
 */
int network_poll_message(char *game_id_out);

/**
 * Check for icon request from desktop
 *
 * @param game_id_out Buffer to receive requested game ID (10 bytes)
 * @return 1 if icon request received, 0 if no request, negative on error
 */
int network_poll_icon_request(char *game_id_out);

/**
 * Force cleanup any existing network state
 * Call this when network_init fails to try to recover
 */
void network_force_cleanup(void);

#endif /* NETWORK_H */
