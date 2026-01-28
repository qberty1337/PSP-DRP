/**
 * Network Communication Implementation
 */

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspwlan.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network.h"
#include "discord_rpc.h"

/* Maximum packet size */
#define MAX_PACKET_SIZE 2048

/* Known return codes */
#define NET_MODULE_ALREADY_LOADED ((int)0x80110F01)
#define NET_ALREADY_INITIALIZED   ((int)0x80410003)

/* Socket file descriptor */
static int g_socket = -1;

/* Desktop address */
static struct sockaddr_in g_desktop_addr;

/* Discovery socket */
static int g_discovery_socket = -1;

/* Plugin start time for uptime calculation */
static SceUInt64 g_start_time = 0;

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

/**
 * Initialize network subsystem
 */
int network_init(void) {
    int ret;
    
    /* Load required modules */
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (ret < 0 && ret != NET_MODULE_ALREADY_LOADED) { /* Already loaded */
        return ret;
    }
    
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (ret < 0 && ret != NET_MODULE_ALREADY_LOADED) {
        return ret;
    }
    
    /* Initialize networking */
    ret = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (ret < 0 && ret != NET_ALREADY_INITIALIZED) { /* Already initialized */
        return ret;
    }
    
    ret = sceNetInetInit();
    if (ret < 0 && ret != NET_ALREADY_INITIALIZED) {
        return ret;
    }
    
    ret = sceNetApctlInit(0x8000, 48);
    if (ret < 0 && ret != NET_ALREADY_INITIALIZED) {
        return ret;
    }
    
    g_start_time = get_time_us();
    
    return 0;
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
    
    sceNetApctlTerm();
    sceNetInetTerm();
    sceNetTerm();
}

/**
 * Connect to desktop companion app
 */
int network_connect(const PluginConfig *config) {
    int ret;
    
    /* Connect to WiFi access point */
    ret = connect_to_ap();
    if (ret < 0) {
        return ret;
    }
    
    /* Wait for connection */
    ret = wait_for_connection(30);
    if (ret < 0) {
        return ret;
    }
    
    /* Create UDP socket */
    g_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket < 0) {
        return g_socket;
    }
    
    /* Set up desktop address */
    memset(&g_desktop_addr, 0, sizeof(g_desktop_addr));
    g_desktop_addr.sin_family = AF_INET;
    g_desktop_addr.sin_port = htons(config->port);
    
    if (config->desktop_ip[0] != '\0') {
        /* Use configured IP */
        g_desktop_addr.sin_addr.s_addr = inet_addr(config->desktop_ip);
    } else if (config->auto_discovery) {
        /* Create discovery socket for listening */
        g_discovery_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
        if (g_discovery_socket >= 0) {
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(DISCOVERY_PORT);
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            
            sceNetInetBind(g_discovery_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        }
        
        /* Will wait for discovery request */
        return 1;
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
 * Show WiFi profile selector UI and connect
 */
int network_show_profile_selector(void) {
    pspUtilityNetconfData data;
    int done = 0;
    int result;

    memset(&data, 0, sizeof(data));
    data.base.size = sizeof(data);

    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &data.base.language);
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
 */
static int connect_to_ap(void) {
    int ret;
    int state;
    
    /* Check if already connected */
    ret = sceNetApctlGetState(&state);
    if (ret == 0 && state == PSP_NET_APCTL_STATE_GOT_IP) {
        return 0;
    }
    
    /* Try to connect to first available connection */
    ret = sceNetApctlConnect(1);
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
    int timeout = timeout_seconds * 10; /* 100ms intervals */
    
    while (timeout > 0) {
        if (sceNetApctlGetState(&state) < 0) {
            return -1;
        }
        
        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            return 0;
        }
        
        sceKernelDelayThread(100 * 1000); /* 100ms */
        timeout--;
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
    
    if (g_socket < 0 || g_desktop_addr.sin_addr.s_addr == 0) {
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
    return sceNetInetSendto(g_socket, buffer, total_size, 0,
                            (struct sockaddr *)&g_desktop_addr, sizeof(g_desktop_addr));
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
    crc32 = 0; /* TODO: Implement CRC32 */
    
    /* Send chunks */
    for (chunk_index = 0; chunk_index < total_chunks; chunk_index++) {
        memset(&chunk, 0, sizeof(chunk));
        copy_str(chunk.game_id, sizeof(chunk.game_id), game_id);
        chunk.chunk_index = chunk_index;
        chunk.total_chunks = total_chunks;
        
        /* Calculate chunk size */
        uint32_t remaining = icon_size - offset;
        chunk.data_length = (remaining > ICON_CHUNK_SIZE) ? ICON_CHUNK_SIZE : remaining;
        
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
    ret = sceNetInetRecvfrom(g_discovery_socket, buffer, sizeof(buffer), MSG_DONTWAIT,
                             (struct sockaddr *)&from_addr, &from_len);
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
    
    /* Get PSP nickname */
    if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME,
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

    /* Save discovered desktop IP if requested */
    if (config != NULL) {
        char ip_str[16];
        ipv4_to_str(from_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
        if (strcmp(config->desktop_ip, ip_str) != 0 || config->port != request->listen_port) {
            copy_str(config->desktop_ip, sizeof(config->desktop_ip), ip_str);
            config->port = request->listen_port;
            config_save(config);
        }
    }
    
    /* Send response */
    if (send_packet(MSG_DISCOVERY_RESPONSE, &response, sizeof(response)) < 0) {
        return -1;
    }

    return 1;
}
