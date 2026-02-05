/**
 * USB Protocol for PSP DRP
 *
 * Packet format for USB communication between PSP and desktop.
 * Compatible with USBHostFS-style architecture.
 */

#ifndef USB_PROTOCOL_H
#define USB_PROTOCOL_H

#include <stdint.h>

/* Packet magic - "PSPD" = 0x50535044 (same as desktop companion) */
#define USB_PACKET_MAGIC 0x50535044

/* Packet types */
#define USB_PKT_HEARTBEAT 0x01
#define USB_PKT_GAME_INFO 0x02
#define USB_PKT_ICON_CHUNK 0x03
#define USB_PKT_STATS_REQUEST 0x05
#define USB_PKT_STATS_UPLOAD 0x06
#define USB_PKT_ACK 0x10
#define USB_PKT_ICON_REQUEST 0x11
#define USB_PKT_STATS_RESPONSE 0x12

/* Maximum packet size for bulk transfers */
#define USB_MAX_PACKET 512

/* Icon chunk maximum data size (leaves room for header) */
#define USB_ICON_CHUNK_SIZE 450

/* Stats chunk maximum data size */
#define USB_STATS_CHUNK_SIZE 480

/* PSP state values */
#define USB_STATE_XMB 0
#define USB_STATE_GAME 1
#define USB_STATE_HOMEBREW 2

/*============================================================================
 * Packet Structures
 * All packets have fixed size to simplify USB transfers
 *============================================================================*/

/* Packet header (8 bytes) */
typedef struct {
  uint32_t magic; /* USB_PACKET_MAGIC */
  uint8_t type;   /* Packet type */
  uint8_t reserved;
  uint16_t length; /* Payload length (not including header) */
} __attribute__((packed)) UsbPacketHeader;

/* Game info packet (128 bytes total) */
typedef struct {
  UsbPacketHeader header;
  char game_id[10];    /* e.g., "UCUS98632" */
  char title[64];      /* Game title */
  uint8_t state;       /* USB_STATE_* */
  uint8_t has_icon;    /* 1 if icon available */
  uint32_t start_time; /* Unix timestamp */
  uint8_t persistent;  /* Keep presence after disconnect */
  char psp_name[32];   /* PSP name */
  uint8_t padding[7];  /* Padding to 128 bytes */
} __attribute__((packed)) UsbGameInfoPacket;

/* Heartbeat packet (16 bytes) */
typedef struct {
  UsbPacketHeader header;
  uint32_t uptime; /* Uptime in seconds */
  uint8_t battery; /* Battery percent */
  uint8_t padding[3];
} __attribute__((packed)) UsbHeartbeatPacket;

/* ACK packet (8 bytes - just header) */
typedef UsbPacketHeader UsbAckPacket;

/* Icon request packet (24 bytes) */
typedef struct {
  UsbPacketHeader header;
  char game_id[10];
  uint8_t padding[6];
} __attribute__((packed)) UsbIconRequestPacket;

/* Icon chunk packet (512 bytes max) */
typedef struct {
  UsbPacketHeader header;
  char game_id[10];                  /* Game ID this icon belongs to */
  uint16_t total_size;               /* Total icon file size */
  uint16_t chunk_offset;             /* Offset of this chunk in file */
  uint16_t chunk_size;               /* Size of data in this chunk */
  uint8_t chunk_num;                 /* Current chunk number (0-based) */
  uint8_t total_chunks;              /* Total number of chunks */
  uint8_t data[USB_ICON_CHUNK_SIZE]; /* Chunk data */
} __attribute__((packed)) UsbIconChunkPacket;

/* Stats request packet (16 bytes) */
typedef struct {
  UsbPacketHeader header;
  uint64_t local_timestamp; /* PSP's last_updated timestamp */
} __attribute__((packed)) UsbStatsRequestPacket;

/* Stats upload packet (512 bytes max) */
typedef struct {
  UsbPacketHeader header;
  uint64_t last_updated;              /* PSP's last_updated timestamp */
  uint16_t chunk_index;               /* Current chunk (0-based) */
  uint16_t total_chunks;              /* Total chunks */
  uint16_t data_length;               /* Length of data in this chunk */
  uint8_t data[USB_STATS_CHUNK_SIZE]; /* JSON chunk data */
} __attribute__((packed)) UsbStatsUploadPacket;

/* Stats response packet (512 bytes max) */
typedef struct {
  UsbPacketHeader header;
  uint64_t last_updated; /* Desktop's last_updated timestamp */
  uint32_t total_bytes;  /* Total bytes in full JSON (for verification) */
  uint16_t chunk_index;  /* Current chunk (0-based) */
  uint16_t total_chunks; /* Total chunks */
  uint16_t data_length;  /* Length of data in this chunk */
  uint8_t data[USB_STATS_CHUNK_SIZE]; /* JSON chunk data */
} __attribute__((packed)) UsbStatsResponsePacket;

/*============================================================================
 * Protocol Functions
 *============================================================================*/

/**
 * Send game info to desktop via USB
 *
 * @param game_id Game ID string
 * @param title Game title
 * @param state Current state (XMB/Game/Homebrew)
 * @param has_icon Whether icon is available
 * @param start_time Unix timestamp when started
 * @param persistent Keep presence after disconnect
 * @param psp_name PSP name from config
 * @return 0 on success, negative on error
 */
int usb_send_game_info(const char *game_id, const char *title, int state,
                       int has_icon, uint32_t start_time, int persistent,
                       const char *psp_name);

/**
 * Send heartbeat to desktop
 *
 * @param uptime Uptime in seconds
 * @param battery Battery percentage
 * @return 0 on success, negative on error
 */
int usb_send_heartbeat(uint32_t uptime, uint8_t battery);

/**
 * Poll for incoming message from desktop
 *
 * @param game_id_out Buffer to receive game ID for icon request (10 bytes)
 * @return Packet type received, or 0 if no message
 */
int usb_poll_message(char *game_id_out);

/**
 * Send icon data to desktop
 *
 * @param game_id Game ID the icon belongs to
 * @param icon_data Pointer to PNG icon data
 * @param icon_size Size of icon data in bytes
 * @return 0 on success, negative on error
 */
int usb_send_icon(const char *game_id, const uint8_t *icon_data,
                  uint32_t icon_size);

/**
 * Send stats request to desktop
 *
 * @param local_timestamp PSP's last_updated timestamp
 * @return 0 on success, negative on error
 */
int usb_send_stats_request(uint64_t local_timestamp);

/**
 * Send stats upload to desktop (chunked)
 *
 * @param json_data JSON data to send
 * @param json_len Length of JSON data
 * @param last_updated PSP's last_updated timestamp
 * @return 0 on success, negative on error
 */
int usb_send_stats_upload(const char *json_data, size_t json_len,
                          uint64_t last_updated);

/**
 * Poll for stats response from desktop
 *
 * @param remote_last_updated Output: desktop's timestamp
 * @param json_buffer Buffer to receive JSON data
 * @param buffer_size Size of json_buffer
 * @param json_len_out Output: length of JSON data received
 * @return 1 if complete, 0 if still receiving, negative on error
 */
int usb_poll_stats_response(uint64_t *remote_last_updated, char *json_buffer,
                            size_t buffer_size, size_t *json_len_out);

#endif /* USB_PROTOCOL_H */
