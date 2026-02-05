/**
 * USB Driver Module
 *
 * Implements USB device mode communication with desktop companion
 * using bulk endpoints (similar to USBHostFS architecture).
 */

#ifndef USB_DRIVER_H
#define USB_DRIVER_H

#include "config.h"
#include "discord_rpc.h"
#include <stdint.h>

/* USB Product identification (Sony PSP Type B - compatible with USBHostFS
 * driver) */
#define USB_VENDOR_ID 0x054C  /* Sony */
#define USB_PRODUCT_ID 0x02E1 /* PSP Type B */

/* USB Endpoints */
#define USB_EP_BULK_IN 0x81  /* PSP -> PC */
#define USB_EP_BULK_OUT 0x02 /* PC -> PSP */

/* Driver PID for sceUsbActivate (same as RemoteJoyLite for compatibility) */
#define USB_DRIVER_PID 0x1C9

/* Max packet size for bulk transfers (high-speed) */
#define USB_MAX_PACKET_SIZE 512

/* Packet header magic */
#define USB_MAGIC 0x50535044 /* "PSPD" */

/* USB Driver States */
typedef enum {
  USB_STATE_UNINITIALIZED = 0,
  USB_STATE_INITIALIZED,
  USB_STATE_CONNECTED,
  USB_STATE_ERROR
} UsbDriverState;

/**
 * Initialize USB driver subsystem
 * Registers USB device driver with the kernel
 *
 * @return 0 on success, negative on error
 */
int usb_driver_init(void);

/**
 * Shutdown USB driver subsystem
 * Unregisters driver and releases resources
 */
void usb_driver_shutdown(void);

/**
 * Start USB communication
 * Activates the USB bus and waits for host connection
 *
 * @return 0 on success, negative on error
 */
int usb_driver_start(void);

/**
 * Stop USB communication
 */
void usb_driver_stop(void);

/**
 * Check if USB is connected to host
 *
 * @return 1 if connected, 0 otherwise
 */
int usb_driver_is_connected(void);

/**
 * Get current USB driver state
 *
 * @return Current state
 */
UsbDriverState usb_driver_get_state(void);

/**
 * Send heartbeat packet over USB
 *
 * @return 0 on success, negative on error
 */
int usb_send_heartbeat(void);

/**
 * Send game info over USB
 *
 * @param info Game information to send
 * @return 0 on success, negative on error
 */
int usb_send_game_info(const GameInfo *info);

/**
 * Send icon data over USB (chunked)
 *
 * @param game_id Game ID this icon belongs to
 * @param icon_data PNG icon data
 * @param icon_size Size of icon data
 * @return 0 on success, negative on error
 */
int usb_send_icon(const char *game_id, const uint8_t *icon_data,
                  uint32_t icon_size);

/**
 * Poll for incoming messages from desktop
 *
 * @param game_id_out Buffer for requested game ID (10 bytes), can be NULL
 * @return 1 if ACK received, 2 if icon request, 0 if nothing, negative on error
 */
int usb_poll_message(char *game_id_out);

/**
 * Send raw data over USB bulk OUT endpoint
 *
 * @param data Data to send
 * @param len Length of data
 * @return Bytes sent on success, negative on error
 */
int usb_bulk_send(const void *data, int len);

/**
 * Receive raw data from USB bulk IN endpoint
 *
 * @param data Buffer for received data
 * @param maxlen Maximum bytes to receive
 * @return Bytes received on success, negative on error
 */
int usb_bulk_recv(void *data, int maxlen);

#endif /* USB_DRIVER_H */
