/**
 * USB Driver Implementation
 *
 * Based on PSPLINK's USBHostFS architecture.
 * Registers a custom USB device with bulk endpoints for
 * communication with the desktop companion.
 */

#include <pspkernel.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <string.h>

#include "discord_rpc.h"
#include "usb_driver.h"

/* Debug logging - forward declare from other module or use simple version */
extern void net_log(const char *fmt, ...);

#ifdef USB_DEBUG
#define USB_LOG(...) net_log(__VA_ARGS__)
#else
#define USB_LOG(...)
#endif

/* USB Driver name */
#define USB_DRIVER_NAME "PSP_DRP_USB"

/* USB Bus driver name (for sceUsbStart) */
#define USB_BUS_DRIVER_NAME "USBBusDriver"

/* USB Event flags */
enum UsbEvents {
  USB_EVENT_ATTACH = 1,
  USB_EVENT_DETACH = 2,
  USB_EVENT_ASYNC = 4,
  USB_EVENT_CONNECT = 8,
  USB_EVENT_ALL = 0xFFFFFFFF
};

/* Transfer event flags */
enum UsbTransEvents {
  USB_TRANS_BULKOUT_DONE = 1,
  USB_TRANS_BULKIN_DONE = 2,
};

/* Global state */
static UsbDriverState g_state = USB_STATE_UNINITIALIZED;
static SceUID g_usb_event = -1;
static SceUID g_trans_event = -1;
static int g_connected = 0;

/* Transfer request structures */
static struct UsbdDeviceReq g_bulkin_req;
static struct UsbdDeviceReq g_bulkout_req;

/* Transfer buffers (aligned for DMA) */
static u8 g_sendbuf[USB_MAX_PACKET_SIZE] __attribute__((aligned(64)));
static u8 g_recvbuf[USB_MAX_PACKET_SIZE] __attribute__((aligned(64)));

/*============================================================================
 * USB Device Descriptors (High-Speed)
 *============================================================================*/

/* Device descriptor */
static struct DeviceDescriptor g_devdesc_hi = {
    .bLength = 18,
    .bDescriptorType = 0x01,
    .bcdUSB = 0x0200,     /* USB 2.0 */
    .bDeviceClass = 0x00, /* Per-interface */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize = 64,
    .idVendor = 0,       /* Kernel fills based on driver PID */
    .idProduct = 0,      /* Kernel fills based on driver PID */
    .bcdDevice = 0x0100, /* Version 1.0 */
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1};

/* Configuration descriptor */
static struct ConfigDescriptor g_confdesc_hi = {
    .bLength = 9,
    .bDescriptorType = 0x02,
    .wTotalLength = (9 + 9 + (2 * 7)), /* Config + Interface + 2 Endpoints */
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0xC0, /* Self-powered */
    .bMaxPower = 0        /* No bus power */
};

/* Interface descriptor */
static struct InterfaceDescriptor g_interdesc_hi = {
    .bLength = 9,
    .bDescriptorType = 0x04,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = 0xFF, /* Vendor specific */
    .bInterfaceSubClass = 0x01,
    .bInterfaceProtocol = 0xFF,
    .iInterface = 1};

/* Endpoint descriptors (high-speed: 512 bytes) */
static struct EndpointDescriptor g_endpdesc_hi[2] __attribute__((
    aligned(16))) = {{.bLength = 7,
                      .bDescriptorType = 0x05,
                      .bEndpointAddress = USB_EP_BULK_IN, /* 0x81 - IN */
                      .bmAttributes = 0x02,               /* Bulk */
                      .wMaxPacketSize = USB_MAX_PACKET_SIZE,
                      .bInterval = 0},
                     {.bLength = 7,
                      .bDescriptorType = 0x05,
                      .bEndpointAddress = USB_EP_BULK_OUT, /* 0x02 - OUT */
                      .bmAttributes = 0x02,                /* Bulk */
                      .wMaxPacketSize = USB_MAX_PACKET_SIZE,
                      .bInterval = 0}};

/*============================================================================
 * USB Device Descriptors (Full-Speed fallback)
 *============================================================================*/

static struct DeviceDescriptor g_devdesc_full = {.bLength = 18,
                                                 .bDescriptorType = 0x01,
                                                 .bcdUSB = 0x0200,
                                                 .bDeviceClass = 0x00,
                                                 .bDeviceSubClass = 0x00,
                                                 .bDeviceProtocol = 0x00,
                                                 .bMaxPacketSize = 64,
                                                 .idVendor = 0,
                                                 .idProduct = 0,
                                                 .bcdDevice = 0x0100,
                                                 .iManufacturer = 0,
                                                 .iProduct = 0,
                                                 .iSerialNumber = 0,
                                                 .bNumConfigurations = 1};

static struct ConfigDescriptor g_confdesc_full = {.bLength = 9,
                                                  .bDescriptorType = 0x02,
                                                  .wTotalLength =
                                                      (9 + 9 + (2 * 7)),
                                                  .bNumInterfaces = 1,
                                                  .bConfigurationValue = 1,
                                                  .iConfiguration = 0,
                                                  .bmAttributes = 0xC0,
                                                  .bMaxPower = 0};

static struct InterfaceDescriptor g_interdesc_full = {
    .bLength = 9,
    .bDescriptorType = 0x04,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = 0xFF,
    .bInterfaceSubClass = 0x01,
    .bInterfaceProtocol = 0xFF,
    .iInterface = 1};

/* Full-speed endpoints (64 bytes max) */
static struct EndpointDescriptor g_endpdesc_full[2]
    __attribute__((aligned(16))) = {{.bLength = 7,
                                     .bDescriptorType = 0x05,
                                     .bEndpointAddress = USB_EP_BULK_IN,
                                     .bmAttributes = 0x02,
                                     .wMaxPacketSize = 64,
                                     .bInterval = 0},
                                    {.bLength = 7,
                                     .bDescriptorType = 0x05,
                                     .bEndpointAddress = USB_EP_BULK_OUT,
                                     .bmAttributes = 0x02,
                                     .wMaxPacketSize = 64,
                                     .bInterval = 0}};

/* String descriptor */
static struct StringDescriptor g_strdesc = {
    .bLength = 8, .bDescriptorType = 0x03, .bString = {'D', 'R', 'P', 0}};

/* Endpoint structures */
static struct UsbEndpoint g_endpoints[3] = {
    {0, 0, 0}, /* Control endpoint */
    {1, 0, 0}, /* Bulk IN */
    {2, 0, 0}, /* Bulk OUT */
};

/* Interface structure */
static struct UsbInterface g_interface = {
    .expect_interface = 0xFFFFFFFF,
    .unk8 = 0,
    .num_interface = 1,
};

/* Interfaces for hi-speed configuration */
static struct UsbInterfaces g_interfaces_hi = {.infp = {&g_interdesc_hi, NULL},
                                               .num = 1};

/* Interfaces for full-speed configuration */
static struct UsbInterfaces g_interfaces_full = {
    .infp = {&g_interdesc_full, NULL}, .num = 1};

/* Hi-speed configuration */
static struct UsbConfiguration g_config_hi = {.confp = &g_confdesc_hi,
                                              .infs = &g_interfaces_hi,
                                              .infp = &g_interdesc_hi,
                                              .endp = g_endpdesc_hi};

/* Full-speed configuration */
static struct UsbConfiguration g_config_full = {.confp = &g_confdesc_full,
                                                .infs = &g_interfaces_full,
                                                .infp = &g_interdesc_full,
                                                .endp = g_endpdesc_full};

/*============================================================================
 * USB Driver Callbacks
 *============================================================================*/

static int usb_recvctl(int arg1, int arg2, struct DeviceRequest *req) {
  USB_LOG("[USB] Request: type=%02X req=%02X val=%04X idx=%04X len=%04X",
          req->bmRequestType, req->bRequest, req->wValue, req->wIndex,
          req->wLength);
  return 0;
}

static int usb_func28(int arg1, int arg2, int arg3) {
  USB_LOG("[USB] func28: arg1=%d arg2=%d arg3=%d", arg1, arg2, arg3);
  return 0;
}

static int usb_attach(int speed, void *arg2, void *arg3) {
  USB_LOG("[USB] Attached at speed %d", speed);
  sceKernelSetEventFlag(g_usb_event, USB_EVENT_ATTACH);
  g_connected = 1;
  return 0;
}

static int usb_detach(int arg1, int arg2, int arg3) {
  USB_LOG("[USB] Detached");
  g_connected = 0;
  sceKernelSetEventFlag(g_usb_event, USB_EVENT_DETACH);
  return 0;
}

/* Bulk transfer callbacks */
static int bulkin_done_cb(struct UsbdDeviceReq *req, int arg1, int arg2) {
  (void)req;
  (void)arg1;
  (void)arg2;
  sceKernelSetEventFlag(g_trans_event, USB_TRANS_BULKIN_DONE);
  return 0;
}

static int bulkout_done_cb(struct UsbdDeviceReq *req, int arg1, int arg2) {
  (void)req;
  (void)arg1;
  (void)arg2;
  sceKernelSetEventFlag(g_trans_event, USB_TRANS_BULKOUT_DONE);
  return 0;
}

/*============================================================================
 * USB Driver Start/Stop Callbacks
 * These are called by sceUsbStart/sceUsbStop for our driver
 *============================================================================*/

static int usb_start_func(int size, void *args) {
  (void)size;
  (void)args;
  USB_LOG("[USB] start_func called");

  /* Create event flags with 0x200 flag (same as RemoteJoyLite) */
  g_usb_event = sceKernelCreateEventFlag("USBEvent", 0x200, 0, NULL);
  if (g_usb_event < 0) {
    USB_LOG("[USB] Failed to create event flag: %08X", g_usb_event);
    return -1;
  }

  g_trans_event = sceKernelCreateEventFlag("USBTransEvent", 0x200, 0, NULL);
  if (g_trans_event < 0) {
    USB_LOG("[USB] Failed to create transfer event flag: %08X", g_trans_event);
    sceKernelDeleteEventFlag(g_usb_event);
    g_usb_event = -1;
    return -1;
  }

  USB_LOG("[USB] start_func completed successfully");
  return 0;
}

static int usb_stop_func(int size, void *args) {
  (void)size;
  (void)args;
  USB_LOG("[USB] stop_func called");

  if (g_trans_event >= 0) {
    sceKernelDeleteEventFlag(g_trans_event);
    g_trans_event = -1;
  }

  if (g_usb_event >= 0) {
    sceKernelDeleteEventFlag(g_usb_event);
    g_usb_event = -1;
  }

  return 0;
}

/*============================================================================
 * USB Driver Structure
 *============================================================================*/

static struct UsbDriver g_usb_driver = {.name = USB_DRIVER_NAME,
                                        .endpoints = 3,
                                        .endp = g_endpoints,
                                        .intp = &g_interface,
                                        .devp_hi = &g_devdesc_hi,
                                        .confp_hi = &g_config_hi,
                                        .devp = &g_devdesc_full,
                                        .confp = &g_config_full,
                                        .str = &g_strdesc,
                                        .recvctl = usb_recvctl,
                                        .func28 = usb_func28,
                                        .attach = usb_attach,
                                        .detach = usb_detach,
                                        .unk34 = 0,
                                        .start_func = usb_start_func,
                                        .stop_func = usb_stop_func,
                                        .link = NULL};

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int usb_driver_init(void) {
  int ret;

  if (g_state != USB_STATE_UNINITIALIZED) {
    USB_LOG("[USB] Driver already initialized");
    return -1;
  }

  USB_LOG("[USB] Initializing driver...");

  /* Start USB bus driver */
  ret = sceUsbStart(USB_BUS_DRIVER_NAME, 0, NULL);
  if (ret < 0) {
    USB_LOG("[USB] Failed to start USB bus driver: %08X", ret);
    return ret;
  }

  /* Register our USB driver */
  ret = sceUsbbdRegister(&g_usb_driver);
  if (ret < 0) {
    USB_LOG("[USB] Failed to register driver: %08X", ret);
    sceUsbStop(USB_BUS_DRIVER_NAME, 0, NULL);
    return ret;
  }

  /* Start our USB driver (this calls start_func which creates event flags) */
  ret = sceUsbStart(USB_DRIVER_NAME, 0, NULL);
  if (ret < 0) {
    USB_LOG("[USB] Failed to start driver: %08X", ret);
    sceUsbbdUnregister(&g_usb_driver);
    sceUsbStop(USB_BUS_DRIVER_NAME, 0, NULL);
    return ret;
  }

  g_state = USB_STATE_INITIALIZED;
  USB_LOG("[USB] Driver initialized successfully");
  return 0;
}

void usb_driver_shutdown(void) {
  if (g_state == USB_STATE_UNINITIALIZED) {
    return;
  }

  USB_LOG("[USB] Shutting down driver...");

  usb_driver_stop();

  sceUsbStop(USB_DRIVER_NAME, 0, NULL);
  sceUsbbdUnregister(&g_usb_driver);
  sceUsbStop(USB_BUS_DRIVER_NAME, 0, NULL);

  if (g_trans_event >= 0) {
    sceKernelDeleteEventFlag(g_trans_event);
    g_trans_event = -1;
  }

  if (g_usb_event >= 0) {
    sceKernelDeleteEventFlag(g_usb_event);
    g_usb_event = -1;
  }

  g_state = USB_STATE_UNINITIALIZED;
  g_connected = 0;
  USB_LOG("[USB] Driver shutdown complete");
}

int usb_driver_start(void) {
  int ret;

  if (g_state != USB_STATE_INITIALIZED) {
    USB_LOG("[USB] Driver not initialized");
    return -1;
  }

  USB_LOG("[USB] Starting...");

  /* Activate USB with driver PID (not USB product ID) */
  ret = sceUsbActivate(USB_DRIVER_PID);
  if (ret < 0) {
    USB_LOG("[USB] Failed to activate: %08X", ret);
    return ret;
  }

  g_state = USB_STATE_CONNECTED;
  USB_LOG("[USB] Activated, waiting for host connection...");
  return 0;
}

void usb_driver_stop(void) {
  if (g_state != USB_STATE_CONNECTED) {
    return;
  }

  USB_LOG("[USB] Stopping...");
  sceUsbDeactivate(USB_DRIVER_PID);
  g_state = USB_STATE_INITIALIZED;
  g_connected = 0;
}

int usb_driver_is_connected(void) { return g_connected; }

UsbDriverState usb_driver_get_state(void) { return g_state; }

/*============================================================================
 * Bulk Transfer Functions
 *============================================================================*/

int usb_bulk_send(const void *data, int len) {
  u32 result;
  int ret;

  if (!g_connected) {
    return -1;
  }

  if (len > USB_MAX_PACKET_SIZE) {
    len = USB_MAX_PACKET_SIZE;
  }

  memcpy(g_sendbuf, data, len);

  /* Clear event flag */
  sceKernelClearEventFlag(g_trans_event, ~USB_TRANS_BULKIN_DONE);

  /* Setup request - note: uses 'endp' not 'endpoint' */
  memset(&g_bulkin_req, 0, sizeof(g_bulkin_req));
  g_bulkin_req.endp = &g_endpoints[1]; /* Bulk IN */
  g_bulkin_req.data = g_sendbuf;
  g_bulkin_req.size = len;
  g_bulkin_req.func = bulkin_done_cb;

  /* Submit request */
  ret = sceUsbbdReqSend(&g_bulkin_req);
  if (ret < 0) {
    USB_LOG("[USB] Bulk send submit failed: %08X", ret);
    return ret;
  }

  /* Wait for completion */
  ret = sceKernelWaitEventFlag(g_trans_event, USB_TRANS_BULKIN_DONE,
                               PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result,
                               NULL);
  if (ret < 0) {
    USB_LOG("[USB] Bulk send wait failed: %08X", ret);
    return ret;
  }

  return len;
}

int usb_bulk_recv(void *data, int maxlen) {
  u32 result;
  int ret;

  if (!g_connected) {
    return -1;
  }

  if (maxlen > USB_MAX_PACKET_SIZE) {
    maxlen = USB_MAX_PACKET_SIZE;
  }

  /* Clear event flag */
  sceKernelClearEventFlag(g_trans_event, ~USB_TRANS_BULKOUT_DONE);

  /* Setup request - note: uses 'endp' not 'endpoint' */
  memset(&g_bulkout_req, 0, sizeof(g_bulkout_req));
  g_bulkout_req.endp = &g_endpoints[2]; /* Bulk OUT */
  g_bulkout_req.data = g_recvbuf;
  g_bulkout_req.size = maxlen;
  g_bulkout_req.func = bulkout_done_cb;

  /* Submit request */
  ret = sceUsbbdReqRecv(&g_bulkout_req);
  if (ret < 0) {
    USB_LOG("[USB] Bulk recv submit failed: %08X", ret);
    return ret;
  }

  /* Wait for completion */
  ret = sceKernelWaitEventFlag(g_trans_event, USB_TRANS_BULKOUT_DONE,
                               PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result,
                               NULL);
  if (ret < 0) {
    USB_LOG("[USB] Bulk recv wait failed: %08X", ret);
    return ret;
  }

  memcpy(data, g_recvbuf, g_bulkout_req.recvsize);
  return g_bulkout_req.recvsize;
}

/*============================================================================
 * Protocol Layer - Send Functions (Placeholders)
 *============================================================================*/

int usb_send_heartbeat(void) {
  /* Placeholder - reuse existing packet format */
  return 0;
}

int usb_send_game_info(const GameInfo *info) {
  /* Placeholder - will serialize GameInfo and call usb_bulk_send */
  (void)info;
  return 0;
}

int usb_send_icon(const char *game_id, const uint8_t *icon_data,
                  uint32_t icon_size) {
  /* Placeholder - chunked icon transfer */
  (void)game_id;
  (void)icon_data;
  (void)icon_size;
  return 0;
}

int usb_poll_message(char *game_id_out) {
  /* Placeholder - non-blocking check for incoming messages */
  (void)game_id_out;
  return 0;
}
