# PSP Discord Rich Presence Protocol

## Overview

Communication between PSP and Desktop uses UDP for low overhead.

- **Data Port**: 9276 (PSP sends status updates here)
- **Discovery Port**: 9277 (broadcast for auto-discovery)

## Packet Format

All packets use a common header:

```
┌─────────────────┬──────────────┬─────────────────────────────────┐
│ Magic (4 bytes) │ Type (1 byte)│ Payload (variable length)       │
│ "PSPR"          │              │                                 │
└─────────────────┴──────────────┴─────────────────────────────────┘
```

## Message Types

### PSP → Desktop

| Type | Name | Description |
|------|------|-------------|
| 0x01 | HEARTBEAT | Keep-alive, sent every 30 seconds |
| 0x02 | GAME_INFO | Current game/app information |
| 0x03 | ICON_CHUNK | Part of game icon (PNG data) |
| 0x04 | ICON_END | Final icon chunk, signals completion |
| 0x21 | DISCOVERY_RESPONSE | Response to discovery broadcast |

### Desktop → PSP

| Type | Name | Description |
|------|------|-------------|
| 0x10 | ACK | Acknowledge received packet |
| 0x20 | DISCOVERY_REQUEST | Broadcast to find PSPs on network |

## Payload Structures

### HEARTBEAT (0x01)
```c
struct Heartbeat {
    uint32_t uptime_seconds;  // Plugin uptime
    uint8_t  wifi_strength;   // 0-100
};
```

### GAME_INFO (0x02)
```c
struct GameInfo {
    char     game_id[10];     // e.g., "UCUS98632" (null-terminated)
    char     title[128];      // Game title (null-terminated)
    uint32_t start_time;      // Unix timestamp when game started
    uint8_t  state;           // 0=XMB, 1=Game, 2=Homebrew, 3=Video, 4=Music
    uint8_t  has_icon;        // 1 if icon will be sent
};
```

### ICON_CHUNK (0x03)
```c
struct IconChunk {
    char     game_id[10];     // Which game this icon is for
    uint16_t chunk_index;     // 0-based chunk number
    uint16_t total_chunks;    // Total number of chunks
    uint16_t data_length;     // Bytes in this chunk (max 1024)
    uint8_t  data[1024];      // PNG data
};
```

### ICON_END (0x04)
```c
struct IconEnd {
    char     game_id[10];     // Which game this icon is for
    uint32_t total_size;      // Total icon size in bytes
    uint32_t crc32;           // CRC32 of complete icon data
};
```

### DISCOVERY_REQUEST (0x20)
```c
struct DiscoveryRequest {
    uint16_t listen_port;     // Port desktop is listening on
    char     version[8];      // Protocol version "1.0.0"
};
```

### DISCOVERY_RESPONSE (0x21)
```c
struct DiscoveryResponse {
    char     psp_name[32];    // PSP system nickname
    char     version[8];      // Plugin version
    uint8_t  battery_percent; // Battery level
};
```

## Flow Examples

### Normal Operation
1. Desktop starts listening on port 9276
2. PSP connects to desktop IP (from config or discovery)
3. PSP sends HEARTBEAT every 30 seconds
4. When game changes, PSP sends GAME_INFO
5. If has_icon=1, PSP sends ICON_CHUNK packets followed by ICON_END
6. Desktop updates Discord Rich Presence

### Auto-Discovery
1. Desktop broadcasts DISCOVERY_REQUEST to 255.255.255.255:9277
2. All PSPs on network respond with DISCOVERY_RESPONSE to desktop's IP
3. Desktop can then connect to discovered PSPs

## Error Handling

- If no HEARTBEAT received for 90 seconds, assume PSP disconnected
- Retry icon transfer if CRC32 mismatch
- Desktop should buffer partial icon data until ICON_END received
