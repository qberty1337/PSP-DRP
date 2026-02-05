/**
 * USB Driver Implementation
 *
 * Based on RemoteJoyLite's USB architecture exactly.
 * Registers a custom USB device with bulk endpoints for
 * communication with the desktop companion.
 */

#include <pspkernel.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <stdio.h>
#include <string.h>

#include "usb_driver.h"

/* Debug logging - writes to log file on memory stick */
#define USB_LOG_FILE "ms0:/psp_drp.log"

/* Log function implementations - declarations in usb_driver.h */
void usb_log_str(const char *msg) {
  SceUID fd;
  int len = 0;
  const char *p = msg;
  while (*p++)
    len++;

  fd = sceIoOpen(USB_LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, msg, len);
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
  }
}

void usb_log_hex(const char *prefix, int val) {
  SceUID fd;
  char buf[64];
  int i = 0;
  const char *p = prefix;
  const char *hex = "0123456789ABCDEF";

  while (*p && i < 48)
    buf[i++] = *p++;
  buf[i++] = '0';
  buf[i++] = 'x';
  buf[i++] = hex[(val >> 28) & 0xF];
  buf[i++] = hex[(val >> 24) & 0xF];
  buf[i++] = hex[(val >> 20) & 0xF];
  buf[i++] = hex[(val >> 16) & 0xF];
  buf[i++] = hex[(val >> 12) & 0xF];
  buf[i++] = hex[(val >> 8) & 0xF];
  buf[i++] = hex[(val >> 4) & 0xF];
  buf[i++] = hex[val & 0xF];
  buf[i++] = '\n';

  fd = sceIoOpen(USB_LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, buf, i);
    sceIoClose(fd);
  }
}

/* Note: USB_LOG and USB_LOG_ERR macros are now defined in usb_driver.h */

/* USB Driver name (must match what sceUsbStart uses) */
#define DRIVER_NAME "PSPDRPDriver"
#define DRIVER_PID (0x1C9)

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

/*============================================================================
 * USB Descriptors (copied exactly from RemoteJoyLite)
 *============================================================================*/

/* Hi-Speed device descriptor */
static struct DeviceDescriptor devdesc_hi = {.bLength = 18,
                                             .bDescriptorType = 0x01,
                                             .bcdUSB = 0x200,
                                             .bDeviceClass = 0,
                                             .bDeviceSubClass = 0,
                                             .bDeviceProtocol = 0,
                                             .bMaxPacketSize = 64,
                                             .idVendor = 0,
                                             .idProduct = 0,
                                             .bcdDevice = 0x100,
                                             .iManufacturer = 0,
                                             .iProduct = 0,
                                             .iSerialNumber = 0,
                                             .bNumConfigurations = 1};

/* Hi-Speed configuration descriptor */
static struct ConfigDescriptor confdesc_hi = {.bLength = 9,
                                              .bDescriptorType = 2,
                                              .wTotalLength = (9 + 9 + (3 * 7)),
                                              .bNumInterfaces = 1,
                                              .bConfigurationValue = 1,
                                              .iConfiguration = 0,
                                              .bmAttributes = 0xC0,
                                              .bMaxPower = 0};

/* Hi-Speed interface descriptor */
static struct InterfaceDescriptor interdesc_hi = {.bLength = 9,
                                                  .bDescriptorType = 4,
                                                  .bInterfaceNumber = 0,
                                                  .bAlternateSetting = 0,
                                                  .bNumEndpoints = 3,
                                                  .bInterfaceClass = 0xFF,
                                                  .bInterfaceSubClass = 0x1,
                                                  .bInterfaceProtocol = 0xFF,
                                                  .iInterface = 1};

/* Hi-Speed endpoint descriptors (3 endpoints like RemoteJoyLite) */
static struct EndpointDescriptor endpdesc_hi[3] = {
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 0x81,
     .bmAttributes = 2,
     .wMaxPacketSize = 512,
     .bInterval = 0},
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 2,
     .bmAttributes = 2,
     .wMaxPacketSize = 512,
     .bInterval = 0},
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 3,
     .bmAttributes = 2,
     .wMaxPacketSize = 512,
     .bInterval = 0},
};

/* Full-Speed device descriptor */
static struct DeviceDescriptor devdesc_full = {.bLength = 18,
                                               .bDescriptorType = 0x01,
                                               .bcdUSB = 0x200,
                                               .bDeviceClass = 0,
                                               .bDeviceSubClass = 0,
                                               .bDeviceProtocol = 0,
                                               .bMaxPacketSize = 64,
                                               .idVendor = 0,
                                               .idProduct = 0,
                                               .bcdDevice = 0x100,
                                               .iManufacturer = 0,
                                               .iProduct = 0,
                                               .iSerialNumber = 0,
                                               .bNumConfigurations = 1};

/* Full-Speed configuration descriptor */
static struct ConfigDescriptor confdesc_full = {.bLength = 9,
                                                .bDescriptorType = 2,
                                                .wTotalLength =
                                                    (9 + 9 + (3 * 7)),
                                                .bNumInterfaces = 1,
                                                .bConfigurationValue = 1,
                                                .iConfiguration = 0,
                                                .bmAttributes = 0xC0,
                                                .bMaxPower = 0};

/* Full-Speed interface descriptor */
static struct InterfaceDescriptor interdesc_full = {.bLength = 9,
                                                    .bDescriptorType = 4,
                                                    .bInterfaceNumber = 0,
                                                    .bAlternateSetting = 0,
                                                    .bNumEndpoints = 3,
                                                    .bInterfaceClass = 0xFF,
                                                    .bInterfaceSubClass = 0x1,
                                                    .bInterfaceProtocol = 0xFF,
                                                    .iInterface = 1};

/* Full-Speed endpoint descriptors */
static struct EndpointDescriptor endpdesc_full[3] = {
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 0x81,
     .bmAttributes = 2,
     .wMaxPacketSize = 64,
     .bInterval = 0},
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 2,
     .bmAttributes = 2,
     .wMaxPacketSize = 64,
     .bInterval = 0},
    {.bLength = 7,
     .bDescriptorType = 5,
     .bEndpointAddress = 3,
     .bmAttributes = 2,
     .wMaxPacketSize = 64,
     .bInterval = 0},
};

/*============================================================================
 * USB Driver Work Structures (exactly like RemoteJoyLite)
 *============================================================================*/

/* 4 endpoints: control + 3 bulk (exactly like RemoteJoyLite) */
static struct UsbEndpoint g_endpoints[4] = {
    {0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};

static struct UsbInterface g_interface = {0xFFFFFFFF, 0, 1};

/* UsbData for hi-speed and full-speed (exactly like RemoteJoyLite) */
static struct UsbData g_usbdata[2];

/* String descriptor */
static struct StringDescriptor g_strdesc = {
    .bLength = 8, .bDescriptorType = 0x03, .bString = {'D', 'R', 'P', 0}};

/* Event flags and state */
static SceUID g_main_event = -1;
static SceUID g_trans_event = -1;
static int g_connected = 0;
static UsbDriverState g_state = USB_STATE_UNINITIALIZED;

/* Transfer request structures */
static struct UsbdDeviceReq g_bulkin_req;
static struct UsbdDeviceReq g_bulkout_req;

/* Transfer buffers (aligned for DMA) */
static u8 g_sendbuf[USB_MAX_PACKET_SIZE] __attribute__((aligned(64)));
static u8 g_recvbuf[USB_MAX_PACKET_SIZE] __attribute__((aligned(64)));

/*============================================================================
 * USB Driver Callbacks
 *============================================================================*/

static int usb_request(int arg1, int arg2, struct DeviceRequest *req) {
  (void)arg1;
  (void)arg2;
  (void)req;
  return 0;
}

static int usb_unknown(int arg1, int arg2, int arg3) {
  (void)arg1;
  (void)arg2;
  (void)arg3;
  return 0;
}

static int usb_attach(int speed, void *arg2, void *arg3) {
  (void)speed;
  (void)arg2;
  (void)arg3;
  USB_LOG("attach callback");
  g_connected = 1;
  sceKernelSetEventFlag(g_main_event, USB_EVENT_ATTACH);
  return 0;
}

static int usb_detach(int arg1, int arg2, int arg3) {
  (void)arg1;
  (void)arg2;
  (void)arg3;
  USB_LOG("detach callback");
  g_connected = 0;
  sceKernelSetEventFlag(g_main_event, USB_EVENT_DETACH);
  return 0;
}

/*============================================================================
 * USB Driver Start/Stop (exactly like RemoteJoyLite)
 *============================================================================*/

static int usb_start_func(int size, void *p) {
  (void)size;
  (void)p;
  USB_LOG("start_func called");

  /* Initialize UsbData structures exactly like RemoteJoyLite */
  memset(g_usbdata, 0, sizeof(g_usbdata));

  /* Hi-speed data */
  memcpy(g_usbdata[0].devdesc, &devdesc_hi, sizeof(devdesc_hi));
  g_usbdata[0].config.pconfdesc = &g_usbdata[0].confdesc;
  g_usbdata[0].config.pinterfaces = &g_usbdata[0].interfaces;
  g_usbdata[0].config.pinterdesc = &g_usbdata[0].interdesc;
  g_usbdata[0].config.pendp = &g_usbdata[0].endp[0];
  memcpy(g_usbdata[0].confdesc.desc, &confdesc_hi, sizeof(confdesc_hi));
  g_usbdata[0].confdesc.pinterfaces = &g_usbdata[0].interfaces;
  g_usbdata[0].interfaces.pinterdesc[0] = &g_usbdata[0].interdesc;
  g_usbdata[0].interfaces.intcount = 1;
  memcpy(g_usbdata[0].interdesc.desc, &interdesc_hi, sizeof(interdesc_hi));
  g_usbdata[0].interdesc.pendp = &g_usbdata[0].endp[0];
  memcpy(g_usbdata[0].endp[0].desc, &endpdesc_hi[0], sizeof(endpdesc_hi[0]));
  memcpy(g_usbdata[0].endp[1].desc, &endpdesc_hi[1], sizeof(endpdesc_hi[1]));
  memcpy(g_usbdata[0].endp[2].desc, &endpdesc_hi[2], sizeof(endpdesc_hi[2]));

  /* Full-speed data */
  memcpy(g_usbdata[1].devdesc, &devdesc_full, sizeof(devdesc_full));
  g_usbdata[1].config.pconfdesc = &g_usbdata[1].confdesc;
  g_usbdata[1].config.pinterfaces = &g_usbdata[1].interfaces;
  g_usbdata[1].config.pinterdesc = &g_usbdata[1].interdesc;
  g_usbdata[1].config.pendp = &g_usbdata[1].endp[0];
  memcpy(g_usbdata[1].confdesc.desc, &confdesc_full, sizeof(confdesc_full));
  g_usbdata[1].confdesc.pinterfaces = &g_usbdata[1].interfaces;
  g_usbdata[1].interfaces.pinterdesc[0] = &g_usbdata[1].interdesc;
  g_usbdata[1].interfaces.intcount = 1;
  memcpy(g_usbdata[1].interdesc.desc, &interdesc_full, sizeof(interdesc_full));
  g_usbdata[1].interdesc.pendp = &g_usbdata[1].endp[0];
  memcpy(g_usbdata[1].endp[0].desc, &endpdesc_full[0],
         sizeof(endpdesc_full[0]));
  memcpy(g_usbdata[1].endp[1].desc, &endpdesc_full[1],
         sizeof(endpdesc_full[1]));
  memcpy(g_usbdata[1].endp[2].desc, &endpdesc_full[2],
         sizeof(endpdesc_full[2]));

  /* Create event flags */
  g_main_event = sceKernelCreateEventFlag("USBMainEvent", 0x200, 0, NULL);
  if (g_main_event < 0) {
    USB_LOG_ERR("Failed to create main event flag", g_main_event);
    return -1;
  }

  g_trans_event = sceKernelCreateEventFlag("USBTransEvent", 0x200, 0, NULL);
  if (g_trans_event < 0) {
    USB_LOG_ERR("Failed to create trans event flag", g_trans_event);
    sceKernelDeleteEventFlag(g_main_event);
    g_main_event = -1;
    return -1;
  }

  USB_LOG("start_func completed");
  return 0;
}

static int usb_stop_func(int size, void *p) {
  (void)size;
  (void)p;
  USB_LOG("stop_func called");

  if (g_trans_event >= 0) {
    sceKernelDeleteEventFlag(g_trans_event);
    g_trans_event = -1;
  }

  if (g_main_event >= 0) {
    sceKernelDeleteEventFlag(g_main_event);
    g_main_event = -1;
  }

  return 0;
}

/*============================================================================
 * USB Driver Structure (exactly like RemoteJoyLite)
 *============================================================================*/

static struct UsbDriver g_usb_driver = {
    DRIVER_NAME,
    4, /* 4 endpoints */
    g_endpoints,
    &g_interface,
    &g_usbdata[0].devdesc[0], /* Hi-speed device descriptor */
    &g_usbdata[0].config,     /* Hi-speed config */
    &g_usbdata[1].devdesc[0], /* Full-speed device descriptor */
    &g_usbdata[1].config,     /* Full-speed config */
    &g_strdesc,
    usb_request,
    usb_unknown,
    usb_attach,
    usb_detach,
    0,
    usb_start_func,
    usb_stop_func,
    NULL};

/*============================================================================
 * Transfer Callbacks
 *============================================================================*/

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
 * Public API Implementation
 *============================================================================*/

int usb_driver_init(void) {
  int ret;

  if (g_state != USB_STATE_UNINITIALIZED) {
    USB_LOG("Driver already initialized");
    return -1;
  }

  USB_LOG("Initializing driver...");

  /* Register driver FIRST (like RemoteJoyLite does in module_start) */
  ret = sceUsbbdRegister(&g_usb_driver);
  if (ret < 0) {
    USB_LOG_ERR("Failed to register driver", ret);
    return ret;
  }

  USB_LOG("Driver registered");
  g_state = USB_STATE_INITIALIZED;
  return 0;
}

int usb_driver_start(void) {
  int ret;

  if (g_state != USB_STATE_INITIALIZED) {
    USB_LOG("Driver not initialized");
    return -1;
  }

  USB_LOG("Starting USB...");

  /* Start bus driver first */
  ret = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
  if (ret < 0) {
    USB_LOG_ERR("Failed to start bus driver", ret);
    return ret;
  }

  USB_LOG("Bus driver started");

  /* Start our driver */
  ret = sceUsbStart(DRIVER_NAME, 0, 0);
  if (ret < 0) {
    USB_LOG_ERR("Failed to start driver", ret);
    return ret;
  }

  USB_LOG("Driver started");

  /* Activate with driver PID */
  ret = sceUsbActivate(DRIVER_PID);
  if (ret < 0) {
    USB_LOG_ERR("Failed to activate", ret);
    return ret;
  }

  g_state = USB_STATE_CONNECTED;

  /* Log initial USB state for debugging */
  int usb_state = sceUsbGetState();
  USB_LOG_ERR("USB activated, initial state", usb_state);
  return 0;
}

void usb_driver_stop(void) {
  if (g_state != USB_STATE_CONNECTED) {
    return;
  }

  USB_LOG("Stopping USB...");

  sceUsbDeactivate(DRIVER_PID);
  sceUsbStop(DRIVER_NAME, 0, 0);
  sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);

  g_state = USB_STATE_INITIALIZED;
  g_connected = 0;
  USB_LOG("USB stopped");
}

void usb_driver_shutdown(void) {
  if (g_state == USB_STATE_UNINITIALIZED) {
    return;
  }

  USB_LOG("Shutting down driver...");

  if (g_state == USB_STATE_CONNECTED) {
    usb_driver_stop();
  }

  sceUsbbdUnregister(&g_usb_driver);

  g_state = USB_STATE_UNINITIALIZED;
  USB_LOG("Driver shutdown complete");
}

int usb_driver_is_connected(void) {
  /* Use sceUsbGetState instead of callback-based g_connected
   * PSP_USB_CONNECTION_ESTABLISHED (0x0020) means host has claimed interface */
  int state = sceUsbGetState();
  return (state & 0x0020) ? 1 : 0;
}

UsbDriverState usb_driver_get_state(void) { return g_state; }

/*============================================================================
 * Bulk Transfer Functions
 *============================================================================*/

int usb_bulk_send(const void *data, int len) {
  u32 result;
  int ret;

  /* Check connection using sceUsbGetState since attach callback may not fire */
  if (!(sceUsbGetState() & 0x0020)) {
    return -1;
  }

  if (len > USB_MAX_PACKET_SIZE) {
    len = USB_MAX_PACKET_SIZE;
  }

  memcpy(g_sendbuf, data, len);
  sceKernelDcacheWritebackRange(g_sendbuf, len);

  sceKernelClearEventFlag(g_trans_event, ~USB_TRANS_BULKIN_DONE);

  memset(&g_bulkin_req, 0, sizeof(g_bulkin_req));
  g_bulkin_req.endp = &g_endpoints[1]; /* Bulk IN */
  g_bulkin_req.data = g_sendbuf;
  g_bulkin_req.size = len;
  g_bulkin_req.func = bulkin_done_cb;

  ret = sceUsbbdReqSend(&g_bulkin_req);
  if (ret < 0) {
    USB_LOG_ERR("Bulk send submit failed", ret);
    return ret;
  }

  ret = sceKernelWaitEventFlag(g_trans_event, USB_TRANS_BULKIN_DONE,
                               PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result,
                               NULL);
  if (ret < 0) {
    USB_LOG_ERR("Bulk send wait failed", ret);
    return ret;
  }

  return len;
}

int usb_bulk_recv(void *data, int maxlen) {
  u32 result;
  int ret;
  u32 upper_size;

  /* Check connection using sceUsbGetState since attach callback may not fire */
  if (!(sceUsbGetState() & 0x0020)) {
    return -1;
  }

  if (maxlen > USB_MAX_PACKET_SIZE) {
    maxlen = USB_MAX_PACKET_SIZE;
  }

  upper_size = (maxlen + 0x3F) & 0xFFFFFFC0;
  sceKernelDcacheInvalidateRange(g_recvbuf, upper_size);

  sceKernelClearEventFlag(g_trans_event, ~USB_TRANS_BULKOUT_DONE);

  memset(&g_bulkout_req, 0, sizeof(g_bulkout_req));
  g_bulkout_req.endp = &g_endpoints[2]; /* Bulk OUT */
  g_bulkout_req.data = g_recvbuf;
  g_bulkout_req.size = maxlen;
  g_bulkout_req.func = bulkout_done_cb;

  ret = sceUsbbdReqRecv(&g_bulkout_req);
  if (ret < 0) {
    USB_LOG_ERR("Bulk recv submit failed", ret);
    return ret;
  }

  ret = sceKernelWaitEventFlag(g_trans_event, USB_TRANS_BULKOUT_DONE,
                               PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result,
                               NULL);
  if (ret < 0) {
    USB_LOG_ERR("Bulk recv wait failed", ret);
    return ret;
  }

  if (g_bulkout_req.retcode == 0 && g_bulkout_req.recvsize > 0) {
    memcpy(data, g_recvbuf, g_bulkout_req.recvsize);
    return g_bulkout_req.recvsize;
  }

  return -1;
}

int usb_driver_send(const void *data, int len) {
  return usb_bulk_send(data, len);
}

int usb_driver_receive(void *data, int len, int timeout_ms) {
  (void)timeout_ms;
  return usb_bulk_recv(data, len);
}
