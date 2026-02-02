/**
 * PSP DRP Plugin - Common Definitions
 */

#ifndef PSP_DRP_H
#define PSP_DRP_H

#include <pspkernel.h>
#include <stdint.h>

/* Protocol constants */
#define PROTOCOL_MAGIC "PSPR"
#define PROTOCOL_VERSION "0.2.0"
#define DEFAULT_PORT 9276
#define DISCOVERY_PORT 9277

/* Message types */
#define MSG_HEARTBEAT 0x01
#define MSG_GAME_INFO 0x02
#define MSG_ICON_CHUNK 0x03
#define MSG_ICON_END 0x04
#define MSG_ACK 0x10
#define MSG_ICON_REQUEST 0x11
#define MSG_DISCOVERY_REQUEST 0x20
#define MSG_DISCOVERY_RESPONSE 0x21

/* PSP state values */
#define STATE_XMB 0
#define STATE_GAME 1
#define STATE_HOMEBREW 2
#define STATE_VIDEO 3
#define STATE_MUSIC 4

/* Icon chunk size */
#define ICON_CHUNK_SIZE 1024

/* Game information structure */
typedef struct {
  char game_id[10];    /* Game ID (e.g., "UCUS98632") */
  char title[128];     /* Game title */
  uint32_t start_time; /* Unix timestamp when game started */
  uint8_t state;       /* Current state (STATE_*) */
  uint8_t has_icon;    /* Whether icon data is available */
  uint8_t
      persistent;    /* Keep presence alive after disconnect (send_once mode) */
  char psp_name[32]; /* PSP name from config */
} GameInfo;

/* Heartbeat packet */
typedef struct {
  uint32_t uptime_seconds;
  uint8_t wifi_strength;
} __attribute__((packed)) HeartbeatPacket;

/* Game info packet */
typedef struct {
  char game_id[10];
  char title[128];
  uint32_t start_time;
  uint8_t state;
  uint8_t has_icon;
  uint8_t persistent;
  char psp_name[32]; /* PSP name from config */
} __attribute__((packed)) GameInfoPacket;

/* Icon chunk packet */
typedef struct {
  char game_id[10];
  uint16_t chunk_index;
  uint16_t total_chunks;
  uint16_t data_length;
  uint8_t data[ICON_CHUNK_SIZE];
} __attribute__((packed)) IconChunkPacket;

/* Icon end packet */
typedef struct {
  char game_id[10];
  uint32_t total_size;
  uint32_t crc32;
} __attribute__((packed)) IconEndPacket;

/* Discovery request packet */
typedef struct {
  uint16_t listen_port;
  char version[8];
} __attribute__((packed)) DiscoveryRequestPacket;

/* Discovery response packet */
typedef struct {
  char psp_name[32];
  char version[8];
  uint8_t battery_percent;
} __attribute__((packed)) DiscoveryResponsePacket;

/* Icon request packet (from desktop) */
typedef struct {
  char game_id[10];
} __attribute__((packed)) IconRequestPacket;

/* Packet header */
typedef struct {
  char magic[4];
  uint8_t type;
} __attribute__((packed)) PacketHeader;

#endif /* PSP_DRP_H */
