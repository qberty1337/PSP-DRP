/**
 * USB Driver Module (Kernel)
 *
 * Implements USB device mode communication with desktop companion
 * using bulk endpoints (similar to RemoteJoyLite architecture).
 */

#ifndef USB_DRIVER_H
#define USB_DRIVER_H

#include <stdint.h>

/*============================================================================
 * Debug Logging
 *============================================================================*/

/* USB log file path */
#define USB_LOG_FILE "ms0:/psp_drp.log"

/* Logging function declarations (implemented in usb_driver.c) */
void usb_log_str(const char *msg);
void usb_log_hex(const char *prefix, int val);

/* Logging flag - set by main.c from config */
extern int g_logging_enabled;

/* Logging macros - check g_logging_enabled flag */
#define USB_LOG(msg)                                                           \
  do {                                                                         \
    if (g_logging_enabled)                                                     \
      usb_log_str("[USB] " msg);                                               \
  } while (0)
#define USB_LOG_ERR(msg, val)                                                  \
  do {                                                                         \
    if (g_logging_enabled)                                                     \
      usb_log_hex("[USB] " msg " ", val);                                      \
  } while (0)

/* USB Product identification (Sony PSP Type B) */
#define USB_VENDOR_ID 0x054C  /* Sony */
#define USB_PRODUCT_ID 0x02E1 /* PSP Type B */

/* USB Endpoints */
#define USB_EP_BULK_IN 0x81  /* PSP -> PC */
#define USB_EP_BULK_OUT 0x02 /* PC -> PSP */

/* Driver PID for sceUsbActivate (same as RemoteJoyLite for compatibility) */
#define USB_DRIVER_PID 0x1C9

/* Max packet size for bulk transfers (high-speed) */
#define USB_MAX_PACKET_SIZE 512

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
 * Send raw data over USB bulk IN endpoint
 *
 * @param data Data to send
 * @param len Length of data
 * @return Bytes sent on success, negative on error
 */
int usb_bulk_send(const void *data, int len);

/**
 * Receive raw data from USB bulk OUT endpoint
 *
 * @param data Buffer for received data
 * @param maxlen Maximum bytes to receive
 * @return Bytes received on success, negative on error
 */
int usb_bulk_recv(void *data, int maxlen);

/**
 * High-level send wrapper
 */
int usb_driver_send(const void *data, int len);

/**
 * High-level receive wrapper with timeout
 */
int usb_driver_receive(void *data, int len, int timeout_ms);

#endif /* USB_DRIVER_H */
