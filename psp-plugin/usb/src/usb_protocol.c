/**
 * USB Protocol Implementation
 *
 * Implements packet sending/receiving over USB.
 */

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <string.h>

/* NOTE: Stats sync disabled in USB kernel mode - stdlib functions not available
 */

#include "usb_driver.h"
#include "usb_protocol.h"

/*============================================================================
 * Stats Streaming State (write chunks directly to file as received)
 *============================================================================*/

#define USAGE_LOG_PATH "ms0:/seplugins/pspdrp/usage_log.json"

static struct {
  SceUID fd;              /* File handle for streaming writes */
  uint32_t total_bytes;   /* Expected total bytes (from first chunk) */
  uint32_t bytes_written; /* Actual bytes written so far */
  uint16_t total_chunks;
  uint16_t received_chunks;
  uint64_t last_updated;
  int active;
  int verified; /* 1 if bytes match, 0 if failed */
} g_stats_stream = {.fd = -1, .active = 0};

/*============================================================================
 * Protocol Implementation
 *============================================================================*/

int usb_send_game_info(const char *game_id, const char *title, int state,
                       int has_icon, uint32_t start_time, int persistent,
                       const char *psp_name) {
  UsbGameInfoPacket pkt;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  memset(&pkt, 0, sizeof(pkt));

  /* Header */
  pkt.header.magic = USB_PACKET_MAGIC;
  pkt.header.type = USB_PKT_GAME_INFO;
  pkt.header.length = sizeof(pkt) - sizeof(UsbPacketHeader);

  /* Payload */
  if (game_id) {
    strncpy(pkt.game_id, game_id, sizeof(pkt.game_id) - 1);
  }
  if (title) {
    strncpy(pkt.title, title, sizeof(pkt.title) - 1);
  }
  pkt.state = (uint8_t)state;
  pkt.has_icon = has_icon ? 1 : 0;
  pkt.start_time = start_time;
  pkt.persistent = persistent ? 1 : 0;
  if (psp_name) {
    strncpy(pkt.psp_name, psp_name, sizeof(pkt.psp_name) - 1);
  }

  return usb_driver_send(&pkt, sizeof(pkt));
}

int usb_send_heartbeat(uint32_t uptime, uint8_t battery) {
  UsbHeartbeatPacket pkt;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  memset(&pkt, 0, sizeof(pkt));

  pkt.header.magic = USB_PACKET_MAGIC;
  pkt.header.type = USB_PKT_HEARTBEAT;
  pkt.header.length = sizeof(pkt) - sizeof(UsbPacketHeader);
  pkt.uptime = uptime;
  pkt.battery = battery;

  return usb_driver_send(&pkt, sizeof(pkt));
}

int usb_poll_message(char *game_id_out) {
  uint8_t buf[USB_MAX_PACKET];
  int ret;
  UsbPacketHeader *hdr;

  if (!usb_driver_is_connected()) {
    return 0;
  }

  /* Non-blocking receive - returns immediately if no data */
  ret = usb_driver_receive(buf, sizeof(buf), 0);
  if (ret <= 0) {
    return 0;
  }

  if (ret < (int)sizeof(UsbPacketHeader)) {
    return 0;
  }

  hdr = (UsbPacketHeader *)buf;
  if (hdr->magic != USB_PACKET_MAGIC) {
    return 0;
  }

  switch (hdr->type) {
  case USB_PKT_ACK:
    return USB_PKT_ACK;

  case USB_PKT_ICON_REQUEST:
    if (game_id_out && ret >= (int)sizeof(UsbIconRequestPacket)) {
      UsbIconRequestPacket *req = (UsbIconRequestPacket *)buf;
      strncpy(game_id_out, req->game_id, 9);
      game_id_out[9] = '\0';
    }
    return USB_PKT_ICON_REQUEST;

  case USB_PKT_STATS_RESPONSE:
    /* Stats response received - stream directly to file */
    if (ret >= (int)sizeof(UsbStatsResponsePacket)) {
      UsbStatsResponsePacket *resp = (UsbStatsResponsePacket *)buf;

      /* First chunk - open file for writing */
      if (resp->chunk_index == 0 || !g_stats_stream.active) {
        /* Close any previous file if still open */
        if (g_stats_stream.fd >= 0) {
          sceIoClose(g_stats_stream.fd);
        }
        /* Open file for writing (truncate) */
        g_stats_stream.fd = sceIoOpen(
            USAGE_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        g_stats_stream.total_bytes = resp->total_bytes;
        g_stats_stream.bytes_written = 0;
        g_stats_stream.total_chunks = resp->total_chunks;
        g_stats_stream.received_chunks = 0;
        g_stats_stream.last_updated = resp->last_updated;
        g_stats_stream.verified = 0;
        g_stats_stream.active = (g_stats_stream.fd >= 0) ? 1 : 0;
      }

      /* Write chunk data directly to file */
      if (g_stats_stream.fd >= 0 && resp->data_length > 0) {
        int written =
            sceIoWrite(g_stats_stream.fd, resp->data, resp->data_length);
        if (written > 0) {
          g_stats_stream.bytes_written += written;
        }
        g_stats_stream.received_chunks++;
      }

      /* Send ACK for this chunk so desktop knows to send next */
      {
        UsbPacketHeader ack;
        memset(&ack, 0, sizeof(ack));
        ack.magic = USB_PACKET_MAGIC;
        ack.type = USB_PKT_ACK;
        ack.length = 0;
        usb_driver_send(&ack, sizeof(ack));
      }

      /* Last chunk - close file and verify */
      if (g_stats_stream.received_chunks >= g_stats_stream.total_chunks &&
          g_stats_stream.fd >= 0) {
        sceIoClose(g_stats_stream.fd);
        g_stats_stream.fd = -1;

        /* Verify byte count matches */
        if (g_stats_stream.bytes_written == g_stats_stream.total_bytes) {
          g_stats_stream.verified = 1;
        } else {
          /* Truncated - delete corrupt file */
          g_stats_stream.verified = 0;
          sceIoRemove(USAGE_LOG_PATH);
        }
      }
    }
    return USB_PKT_STATS_RESPONSE;

  default:
    return 0;
  }
}

int usb_send_icon(const char *game_id, const uint8_t *icon_data,
                  uint32_t icon_size) {
  UsbIconChunkPacket pkt;
  int chunk_num = 0;
  int total_chunks;
  uint32_t offset = 0;
  int chunk_size;
  int ret;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  if (!icon_data || icon_size == 0 || icon_size > 65535) {
    return -2; /* Invalid data or too large */
  }

  /* Calculate total chunks needed */
  total_chunks = (icon_size + USB_ICON_CHUNK_SIZE - 1) / USB_ICON_CHUNK_SIZE;
  if (total_chunks > 255) {
    return -3; /* Too many chunks */
  }

  /* Send icon in chunks */
  while (offset < icon_size) {
    chunk_size = icon_size - offset;
    if (chunk_size > USB_ICON_CHUNK_SIZE) {
      chunk_size = USB_ICON_CHUNK_SIZE;
    }

    memset(&pkt, 0, sizeof(pkt));

    /* Header */
    pkt.header.magic = USB_PACKET_MAGIC;
    pkt.header.type = USB_PKT_ICON_CHUNK;
    pkt.header.length = sizeof(pkt) - sizeof(UsbPacketHeader);

    /* Chunk info */
    if (game_id) {
      strncpy(pkt.game_id, game_id, sizeof(pkt.game_id) - 1);
    }
    pkt.total_size = (uint16_t)icon_size;
    pkt.chunk_offset = (uint16_t)offset;
    pkt.chunk_size = (uint16_t)chunk_size;
    pkt.chunk_num = (uint8_t)chunk_num;
    pkt.total_chunks = (uint8_t)total_chunks;

    /* Copy chunk data */
    memcpy(pkt.data, icon_data + offset, chunk_size);

    /* Send this chunk */
    ret = usb_driver_send(&pkt, sizeof(pkt));
    if (ret < 0) {
      return ret;
    }

    /* Small delay between chunks to avoid overwhelming USB */
    sceKernelDelayThread(5 * 1000); /* 5ms */

    offset += chunk_size;
    chunk_num++;
  }

  return 0;
}

/*============================================================================
 * Stats Sync Implementation (kernel-safe with static buffers)
 * Note: g_stats_buffer is defined at top of file for use by usb_poll_message
 *============================================================================*/

int usb_send_stats_request(uint64_t local_timestamp) {
  UsbStatsRequestPacket pkt;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  memset(&pkt, 0, sizeof(pkt));
  pkt.header.magic = USB_PACKET_MAGIC;
  pkt.header.type = USB_PKT_STATS_REQUEST;
  pkt.header.length = sizeof(pkt) - sizeof(UsbPacketHeader);
  pkt.local_timestamp = local_timestamp;

  return usb_driver_send(&pkt, sizeof(pkt));
}

int usb_send_stats_upload(const char *json_data, size_t json_len,
                          uint64_t last_updated) {
  UsbStatsUploadPacket pkt;
  size_t offset = 0;
  int chunk_num = 0;
  int total_chunks;
  int ret;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  if (!json_data || json_len == 0) {
    return -2;
  }

  total_chunks = (json_len + USB_STATS_CHUNK_SIZE - 1) / USB_STATS_CHUNK_SIZE;
  if (total_chunks > 65535) {
    return -3;
  }

  while (offset < json_len) {
    size_t chunk_size = json_len - offset;
    if (chunk_size > USB_STATS_CHUNK_SIZE) {
      chunk_size = USB_STATS_CHUNK_SIZE;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.header.magic = USB_PACKET_MAGIC;
    pkt.header.type = USB_PKT_STATS_UPLOAD;
    pkt.header.length = sizeof(pkt) - sizeof(UsbPacketHeader);
    pkt.last_updated = last_updated;
    pkt.chunk_index = (uint16_t)chunk_num;
    pkt.total_chunks = (uint16_t)total_chunks;
    pkt.data_length = (uint16_t)chunk_size;
    memcpy(pkt.data, json_data + offset, chunk_size);

    ret = usb_driver_send(&pkt, sizeof(pkt));
    if (ret < 0) {
      return ret;
    }

    sceKernelDelayThread(10 * 1000); /* 10ms between chunks */

    offset += chunk_size;
    chunk_num++;
  }

  return 0;
}

int usb_poll_stats_response(uint64_t *remote_last_updated, char *json_buffer,
                            size_t buffer_size, size_t *json_len_out) {
  /*
   * With streaming, data is written directly to file as chunks arrive.
   * This function just checks if streaming is complete.
   * The caller doesn't need the buffer since data is already on disk.
   */
  (void)json_buffer;
  (void)buffer_size;

  if (!usb_driver_is_connected()) {
    return -1;
  }

  /* Check if we have any data streaming */
  if (!g_stats_stream.active) {
    return -1; /* No data received yet */
  }

  /* Check if complete (all chunks received and file closed) */
  if (g_stats_stream.received_chunks >= g_stats_stream.total_chunks) {
    *remote_last_updated = g_stats_stream.last_updated;
    *json_len_out = g_stats_stream.bytes_written;

    int verified = g_stats_stream.verified;
    g_stats_stream.active = 0;

    if (verified) {
      return 1; /* Complete and verified */
    } else {
      return -2; /* Truncated - file deleted */
    }
  }

  return 0; /* Still receiving chunks */
}
