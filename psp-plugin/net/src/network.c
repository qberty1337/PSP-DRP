/**
 * Network Communication Implementation
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "discord_rpc.h"
#include "network.h"

/* Maximum packet size */
#define MAX_PACKET_SIZE 2048

/* Known return codes */
#define NET_MODULE_ALREADY_LOADED ((int)0x80110F01)
#define NET_ALREADY_INITIALIZED ((int)0x80410003)
#define NET_LIBRARY_ALREADY_LOADED ((int)0x80110802)

/* Socket file descriptor */
static int g_socket = -1;

/* Desktop address */
static struct sockaddr_in g_desktop_addr;

/* Discovery socket */
static int g_discovery_socket = -1;

/* Plugin start time for uptime calculation */
static SceUInt64 g_start_time = 0;

/* Selected profile ID */
static int g_profile_id = 1;

/* Forward declarations */
static int connect_to_ap(void);
static int wait_for_connection(int timeout_seconds);
static void copy_str(char *dst, size_t dst_size, const char *src);
static void ipv4_to_str(uint32_t addr_nbo, char *out, size_t out_size);

/**
 * Get current time in microseconds
 */
static SceUInt64 get_time_us(void) {
  SceKernelSysClock clock;
  sceKernelGetSystemTime(&clock);
  return clock.low + ((SceUInt64)clock.hi << 32);
}

/**
 * Safe string copy with null termination
 */
static void copy_str(char *dst, size_t dst_size, const char *src) {
  size_t len;
  if (dst == NULL || dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void ipv4_to_str(uint32_t addr_nbo, char *out, size_t out_size) {
  uint32_t ip = ntohl(addr_nbo);
  unsigned int b1 = (ip >> 24) & 0xFF;
  unsigned int b2 = (ip >> 16) & 0xFF;
  unsigned int b3 = (ip >> 8) & 0xFF;
  unsigned int b4 = ip & 0xFF;
  if (out == NULL || out_size == 0) {
    return;
  }
  snprintf(out, out_size, "%u.%u.%u.%u", b1, b2, b3, b4);
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xFFFFFFFF;
  uint32_t i;
  uint32_t j;
  if (data == NULL) {
    return 0;
  }
  for (i = 0; i < len; i++) {
    crc ^= data[i];
    for (j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFF;
}

/**
 * Initialize network subsystem
 */
int network_init(void) {
  int ret;
  int state;

  ret = sceNetApctlGetState(&state);
  net_log("network_init: apctl ret=%d state=%d", ret, state);
  if (ret == 0) {
    g_start_time = get_time_us();
    return 0;
  }

  /* Load required modules */
  ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
  net_log("load NET_COMMON ret=0x%08X", (unsigned int)ret);
  if (ret < 0 && ret != NET_MODULE_ALREADY_LOADED &&
      ret != NET_LIBRARY_ALREADY_LOADED) { /* Already loaded */
    net_log("load NET_COMMON failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
  net_log("load NET_INET ret=0x%08X", (unsigned int)ret);
  if (ret < 0 && ret != NET_MODULE_ALREADY_LOADED &&
      ret != NET_LIBRARY_ALREADY_LOADED) {
    net_log("load NET_INET failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  /* Initialize networking */
  ret = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
  net_log("sceNetInit ret=0x%08X", (unsigned int)ret);
  if (ret < 0 && ret != NET_ALREADY_INITIALIZED) { /* Already initialized */
    net_log("sceNetInit failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  ret = sceNetInetInit();
  net_log("sceNetInetInit ret=0x%08X", (unsigned int)ret);
  if (ret < 0 && ret != NET_ALREADY_INITIALIZED) {
    net_log("sceNetInetInit failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  ret = sceNetApctlInit(0x8000, 48);
  net_log("sceNetApctlInit ret=0x%08X", (unsigned int)ret);
  if (ret < 0 && ret != NET_ALREADY_INITIALIZED) {
    net_log("sceNetApctlInit failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  g_start_time = get_time_us();

  return 0;
}

void network_set_profile_id(int profile_id) {
  if (profile_id > 0) {
    g_profile_id = profile_id;
  } else {
    g_profile_id = 1;
  }
}

/**
 * Shutdown network subsystem
 */
void network_shutdown(void) {
  if (g_socket >= 0) {
    sceNetInetClose(g_socket);
    g_socket = -1;
  }

  if (g_discovery_socket >= 0) {
    sceNetInetClose(g_discovery_socket);
    g_discovery_socket = -1;
  }
  sceNetApctlDisconnect();
  sceNetApctlTerm();
  sceNetInetTerm();
  sceNetTerm();
  sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
  sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

/**
 * Force cleanup any existing network state
 * Call this when network_init fails to try to recover
 */
void network_force_cleanup(void) {
  int ret;
  net_log("force_cleanup: attempting network takeover");

  /* Try to disconnect any existing connection */
  ret = sceNetApctlDisconnect();
  net_log("force_cleanup: disconnect ret=0x%08X", (unsigned int)ret);
  sceKernelDelayThread(100 * 1000);

  /* Try to terminate network subsystems (ignore errors) */
  ret = sceNetApctlTerm();
  net_log("force_cleanup: apctl_term ret=0x%08X", (unsigned int)ret);
  ret = sceNetInetTerm();
  net_log("force_cleanup: inet_term ret=0x%08X", (unsigned int)ret);
  ret = sceNetTerm();
  net_log("force_cleanup: net_term ret=0x%08X", (unsigned int)ret);

  /* Try to unload modules (ignore errors) */
  ret = sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
  net_log("force_cleanup: unload_inet ret=0x%08X", (unsigned int)ret);
  ret = sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
  net_log("force_cleanup: unload_common ret=0x%08X", (unsigned int)ret);

  /* Give the system time to clean up */
  sceKernelDelayThread(500 * 1000);

  net_log("force_cleanup: done");
}

/**
 * Connect to desktop companion app
 */
int network_connect(const PluginConfig *config) {
  int ret;

  net_log("network_connect begin ip=%s port=%d auto=%d", config->desktop_ip,
          config->port, config->auto_discovery);

  /* Connect to WiFi access point */
  ret = connect_to_ap();
  if (ret < 0) {
    net_log("connect_to_ap failed: 0x%08X", (unsigned int)ret);
    return ret;
  }

  /* Wait for connection */
  ret = wait_for_connection(30);
  if (ret < 0) {
    net_log("wait_for_connection failed: %d", ret);
    return ret;
  }

  /* Create UDP socket */
  g_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
  net_log("socket ret=%d", g_socket);
  if (g_socket < 0) {
    return g_socket;
  }
  {
    int enable = 1;
    int opt = sceNetInetSetsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &enable,
                                   sizeof(enable));
    net_log("socket broadcast ret=%d", opt);
  }

  /* Set up desktop address */
  memset(&g_desktop_addr, 0, sizeof(g_desktop_addr));
  g_desktop_addr.sin_family = AF_INET;
  {
    uint16_t port = config->port;
    if (port == 0) {
      port = DEFAULT_PORT;
    }
    g_desktop_addr.sin_port = htons(port);
  }

  if (config->desktop_ip[0] != '\0') {
    /* Use configured IP */
    g_desktop_addr.sin_addr.s_addr = inet_addr(config->desktop_ip);
    net_log("desktop addr set to %s:%d", config->desktop_ip, config->port);
  } else if (config->auto_discovery) {
    /* Create discovery socket for listening */
    g_discovery_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    net_log("discovery socket ret=%d", g_discovery_socket);
    if (g_discovery_socket >= 0) {
      struct sockaddr_in bind_addr;
      memset(&bind_addr, 0, sizeof(bind_addr));
      bind_addr.sin_family = AF_INET;
      bind_addr.sin_port = htons(DISCOVERY_PORT);
      bind_addr.sin_addr.s_addr = INADDR_ANY;

      ret = sceNetInetBind(g_discovery_socket, (struct sockaddr *)&bind_addr,
                           sizeof(bind_addr));
      net_log("discovery bind ret=%d", ret);
    }

    g_desktop_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    net_log("auto-discovery broadcast mode enabled");
    return 0;
  } else {
    return -2;
  }

  return 0;
}

/**
 * Disconnect from desktop app
 */
void network_disconnect(void) {
  if (g_socket >= 0) {
    sceNetInetClose(g_socket);
    g_socket = -1;
  }

  if (g_discovery_socket >= 0) {
    sceNetInetClose(g_discovery_socket);
    g_discovery_socket = -1;
  }

  sceNetApctlDisconnect();
}

/**
 * Poll for incoming messages (ACK or icon request)
 * Returns: 1 = ACK received, 2 = icon request received, 0 = nothing
 * If icon request, game_id_out will be filled
 */
int network_poll_message(char *game_id_out) {
  uint8_t buffer[32];
  struct sockaddr_in from_addr;
  socklen_t from_len = sizeof(from_addr);
  int ret;

  if (g_socket < 0) {
    return 0;
  }

  ret = sceNetInetRecvfrom(g_socket, buffer, sizeof(buffer), MSG_DONTWAIT,
                           (struct sockaddr *)&from_addr, &from_len);
  if (ret <= 0) {
    return 0;
  }

  if (ret < (int)sizeof(PacketHeader)) {
    return 0;
  }

  if (memcmp(buffer, PROTOCOL_MAGIC, 4) != 0) {
    return 0;
  }

  /* Handle based on message type */
  if (buffer[4] == MSG_ACK) {
    g_desktop_addr.sin_family = AF_INET;
    g_desktop_addr.sin_addr = from_addr.sin_addr;
    g_desktop_addr.sin_port = from_addr.sin_port;
    return 1;
  }

  if (buffer[4] == MSG_ICON_REQUEST) {
    if (ret >= (int)(sizeof(PacketHeader) + sizeof(IconRequestPacket))) {
      if (game_id_out != NULL) {
        IconRequestPacket *request =
            (IconRequestPacket *)(buffer + sizeof(PacketHeader));
        copy_str(game_id_out, 10, request->game_id);
        net_log("Icon request received for: %s", game_id_out);
      }
      return 2;
    }
  }

  return 0;
}

/* Legacy wrapper for ACK polling */
int network_poll_ack(void) {
  char game_id[10];
  int result = network_poll_message(game_id);
  return (result == 1) ? 1 : 0;
}

/* Legacy wrapper for icon request polling */
int network_poll_icon_request(char *game_id_out) {
  int result = network_poll_message(game_id_out);
  return (result == 2) ? 1 : 0;
}

/**
 * Show WiFi profile selector UI and connect
 */
int network_show_profile_selector(void) {
  pspUtilityNetconfData data;
  int done = 0;
  int result;

  memset(&data, 0, sizeof(data));
  data.base.size = sizeof(data);

  sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                              &data.base.language);
  data.base.buttonSwap = 0;

  data.base.graphicsThread = 0x11;
  data.base.accessThread = 0x13;
  data.base.fontThread = 0x12;
  data.base.soundThread = 0x10;

  data.action = PSP_NETCONF_ACTION_CONNECTAP;

  if (sceUtilityNetconfInitStart(&data) < 0) {
    return -1;
  }

  while (!done) {
    int status = sceUtilityNetconfGetStatus();
    switch (status) {
    case PSP_UTILITY_DIALOG_VISIBLE:
      sceUtilityNetconfUpdate(1);
      break;
    case PSP_UTILITY_DIALOG_FINISHED:
      sceUtilityNetconfShutdownStart();
      break;
    case PSP_UTILITY_DIALOG_NONE:
      done = 1;
      break;
    default:
      break;
    }
    sceDisplayWaitVblankStart();
  }

  result = data.base.result;
  if (result != 0) {
    return -1;
  }

  if (wait_for_connection(30) < 0) {
    return -1;
  }

  return 0;
}

/**
 * Connect to WiFi access point
 *
 * This function handles network connection with care to not interfere with
 * game connections. If the network is already connected or in the process
 * of connecting (by the game), we reuse that connection instead of
 * disconnecting and reconnecting.
 */
static int connect_to_ap(void) {
  int ret;
  int state = 0;

  net_log("connect_to_ap begin");

  /* Check current network state */
  ret = sceNetApctlGetState(&state);
  net_log("connect_to_ap state ret=%d state=%d", ret, state);

  if (ret == 0) {
    /* Already have an IP - fully connected, reuse this connection */
    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
      net_log("connect_to_ap: already connected, reusing");
      return 0;
    }

    /* Network state > 0 means the game is connecting or connected
     * States: 0=disconnected, 1=scanning, 2=joining, 3=getting IP, 4=got IP
     * For states 1-3, let the game's connection attempt complete */
    if (state > 0) {
      net_log("connect_to_ap: game connecting (state=%d), waiting for it",
              state);
      return 0; /* Let wait_for_connection handle it */
    }
  }

  /* Only disconnect and reconnect if truly disconnected or in error state */
  net_log("connect_to_ap: disconnected, initiating connection");

  /* Ensure clean state before connecting */
  {
    int disc = sceNetApctlDisconnect();
    net_log("connect_to_ap pre-disconnect ret=0x%08X", (unsigned int)disc);
    sceKernelDelayThread(500 * 1000);
  }

  /* Try to connect to configured profile */
  ret = sceNetApctlConnect(g_profile_id);
  net_log("connect_to_ap connect ret=0x%08X profile=%d", (unsigned int)ret,
          g_profile_id);

  if (ret < 0) {
    return ret;
  }

  return 0;
}

/**
 * Wait for network connection
 */
static int wait_for_connection(int timeout_seconds) {
  int state = PSP_NET_APCTL_STATE_DISCONNECTED;
  int timeout = timeout_seconds * 4; /* 300ms intervals */
  int last_state = -1;

  net_log("wait_for_connection timeout=%d", timeout_seconds);

  while (timeout > 0) {
    if (sceNetApctlGetState(&state) < 0) {
      return -1;
    }

    if (state != last_state) {
      net_log("apctl state=%d", state);
      last_state = state;
    }

    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
      return 0;
    }

    sceKernelDelayThread(300 * 1000);
    timeout--;
  }

  net_log("wait_for_connection timeout");
  {
    int disc = sceNetApctlDisconnect();
    net_log("wait_for_connection disconnect ret=0x%08X", (unsigned int)disc);
  }

  return -1; /* Timeout */
}

/**
 * Build and send a packet
 */
static int send_packet(uint8_t type, const void *payload, size_t payload_size) {
  uint8_t buffer[MAX_PACKET_SIZE];
  PacketHeader *header = (PacketHeader *)buffer;
  size_t total_size;
  int ret;

  if (g_socket < 0 || g_desktop_addr.sin_addr.s_addr == 0) {
    net_log("send_packet failed: socket=%d addr=0x%08X", g_socket,
            (unsigned int)g_desktop_addr.sin_addr.s_addr);
    return -1;
  }

  /* Build header */
  memcpy(header->magic, PROTOCOL_MAGIC, 4);
  header->type = type;

  /* Copy payload */
  if (payload && payload_size > 0) {
    if (payload_size > MAX_PACKET_SIZE - sizeof(PacketHeader)) {
      return -2;
    }
    memcpy(buffer + sizeof(PacketHeader), payload, payload_size);
  }

  total_size = sizeof(PacketHeader) + payload_size;

  /* Send packet */
  ret = sceNetInetSendto(g_socket, buffer, total_size, 0,
                         (struct sockaddr *)&g_desktop_addr,
                         sizeof(g_desktop_addr));
  if (ret < 0) {
    net_log("send_packet type=%d failed: %d", type, ret);
  }
  return ret;
}

/**
 * Send heartbeat packet
 */
int network_send_heartbeat(void) {
  HeartbeatPacket packet;
  SceUInt64 now = get_time_us();

  packet.uptime_seconds = (now - g_start_time) / 1000000;
  packet.wifi_strength = 100; /* TODO: Get actual signal strength */

  return send_packet(MSG_HEARTBEAT, &packet, sizeof(packet));
}

/**
 * Send game info update
 */
int network_send_game_info(const GameInfo *info) {
  GameInfoPacket packet;

  memset(&packet, 0, sizeof(packet));
  copy_str(packet.game_id, sizeof(packet.game_id), info->game_id);
  copy_str(packet.title, sizeof(packet.title), info->title);
  packet.start_time = info->start_time;
  packet.state = info->state;
  packet.has_icon = info->has_icon;
  packet.persistent = info->persistent;
  copy_str(packet.psp_name, sizeof(packet.psp_name), info->psp_name);

  return send_packet(MSG_GAME_INFO, &packet, sizeof(packet));
}

/**
 * Send icon data (chunked)
 */
int network_send_icon(const char *game_id, const uint8_t *icon_data,
                      uint32_t icon_size) {
  IconChunkPacket chunk;
  IconEndPacket end;
  uint16_t total_chunks;
  uint16_t chunk_index;
  uint32_t offset = 0;
  uint32_t crc32;
  int ret;

  if (icon_data == NULL || icon_size == 0) {
    return -1;
  }

  /* Calculate total chunks */
  total_chunks = (icon_size + ICON_CHUNK_SIZE - 1) / ICON_CHUNK_SIZE;

  /* Calculate CRC32 */
  crc32 = crc32_calc(icon_data, icon_size);

  /* Send chunks */
  for (chunk_index = 0; chunk_index < total_chunks; chunk_index++) {
    memset(&chunk, 0, sizeof(chunk));
    copy_str(chunk.game_id, sizeof(chunk.game_id), game_id);
    chunk.chunk_index = chunk_index;
    chunk.total_chunks = total_chunks;

    /* Calculate chunk size */
    uint32_t remaining = icon_size - offset;
    chunk.data_length =
        (remaining > ICON_CHUNK_SIZE) ? ICON_CHUNK_SIZE : remaining;

    memcpy(chunk.data, icon_data + offset, chunk.data_length);
    offset += chunk.data_length;

    ret = send_packet(MSG_ICON_CHUNK, &chunk,
                      sizeof(chunk) - ICON_CHUNK_SIZE + chunk.data_length);
    if (ret < 0) {
      return ret;
    }

    /* Small delay between chunks to avoid flooding */
    sceKernelDelayThread(10 * 1000); /* 10ms */
  }

  /* Send end marker */
  memset(&end, 0, sizeof(end));
  copy_str(end.game_id, sizeof(end.game_id), game_id);
  end.total_size = icon_size;
  end.crc32 = crc32;

  return send_packet(MSG_ICON_END, &end, sizeof(end));
}

/**
 * Handle discovery requests
 */
int network_handle_discovery(PluginConfig *config) {
  uint8_t buffer[256];
  struct sockaddr_in from_addr;
  socklen_t from_len = sizeof(from_addr);
  int ret;
  PacketHeader *header;
  DiscoveryRequestPacket *request;
  DiscoveryResponsePacket response;
  char psp_name[32];

  if (g_discovery_socket < 0) {
    return 0;
  }

  /* Non-blocking receive */
  ret = sceNetInetRecvfrom(g_discovery_socket, buffer, sizeof(buffer),
                           MSG_DONTWAIT, (struct sockaddr *)&from_addr,
                           &from_len);
  if (ret <= 0) {
    return 0;
  }

  /* Validate packet */
  if (ret < (int)sizeof(PacketHeader)) {
    return -1;
  }

  header = (PacketHeader *)buffer;
  if (memcmp(header->magic, PROTOCOL_MAGIC, 4) != 0) {
    return -1;
  }

  if (header->type != MSG_DISCOVERY_REQUEST) {
    return -1;
  }

  request = (DiscoveryRequestPacket *)(buffer + sizeof(PacketHeader));

  /* Build response */
  memset(&response, 0, sizeof(response));

  /* Use PSP name from config, or fall back to system nickname */
  if (config != NULL && config->psp_name[0] != '\0') {
    copy_str(response.psp_name, sizeof(response.psp_name), config->psp_name);
  } else if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME,
                                            psp_name, sizeof(psp_name)) == 0) {
    copy_str(response.psp_name, sizeof(response.psp_name), psp_name);
  } else {
    strcpy(response.psp_name, "PSP");
  }

  copy_str(response.version, sizeof(response.version), PROTOCOL_VERSION);
  response.battery_percent = scePowerGetBatteryLifePercent();

  /* Update desktop address */
  g_desktop_addr.sin_family = AF_INET;
  g_desktop_addr.sin_port = htons(request->listen_port);
  g_desktop_addr.sin_addr = from_addr.sin_addr;

  /* Save discovered desktop IP and disable auto-discovery */
  if (config != NULL) {
    char ip_str[16];
    ipv4_to_str(from_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
    if (strcmp(config->desktop_ip, ip_str) != 0 ||
        config->port != request->listen_port || config->auto_discovery != 0) {
      copy_str(config->desktop_ip, sizeof(config->desktop_ip), ip_str);
      config->port = request->listen_port;
      config->auto_discovery = 0;
      {
        int save_res = config_save(config);
        net_log("config_save result=%d ip=%s port=%d auto=%d", save_res,
                config->desktop_ip, config->port, config->auto_discovery);
      }
    }
  }

  /* Send response */
  if (send_packet(MSG_DISCOVERY_RESPONSE, &response, sizeof(response)) < 0) {
    return -1;
  }

  return 1;
}
