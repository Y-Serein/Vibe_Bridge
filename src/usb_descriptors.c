#include <stdint.h>
#include <string.h>

#include "sdk.h"
#include "tusb.h"

#define USB_VID 0x359F
#define USB_PID 0x4203

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC,
};

enum {
    ITF_NUM_NCM = 0,
    ITF_NUM_NCM_DATA,
    ITF_NUM_TOTAL,
};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0201,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t* tud_descriptor_device_cb(void)
{
    return (const uint8_t*)&desc_device;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
#define EPNUM_NET_NOTIF 0x81
#define EPNUM_NET_OUT   0x02
#define EPNUM_NET_IN    0x82

// TinyUSB PR #3630 adds the final capability argument to this descriptor.
// Keep the compatibility definition local until Pico SDK updates TinyUSB.
#define NCM_NETWORK_CAPS_IOS26 ((1u << 0) | (1u << 5))
#define TUD_CDC_NCM_DESCRIPTOR_WITH_CAPS(_itfnum, _desc_stridx, _mac_stridx, \
                                          _ep_notif, _ep_notif_size, _epout, \
                                          _epin, _epsize, _maxsegmentsize, \
                                          _capability) \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC, \
        CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, 0, \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_CDC, \
        CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, _desc_stridx, \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0110), \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum, \
        (uint8_t)((_itfnum) + 1), \
    13, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ETHERNET_NETWORKING, \
        _mac_stridx, 0, 0, 0, 0, U16_TO_U8S_LE(_maxsegmentsize), \
        U16_TO_U8S_LE(0), 0, \
    6, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_NCM, U16_TO_U8S_LE(0x0100), \
        _capability, \
    7, TUSB_DESC_ENDPOINT, _ep_notif, TUSB_XFER_INTERRUPT, \
        U16_TO_U8S_LE(_ep_notif_size), 50, \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 0, 0, \
        TUSB_CLASS_CDC_DATA, 0, NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0, \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 1, 2, \
        TUSB_CLASS_CDC_DATA, 0, NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0, \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, \
        U16_TO_U8S_LE(_epsize), 0, \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, \
        U16_TO_U8S_LE(_epsize), 0

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR_WITH_CAPS(ITF_NUM_NCM,
                                     STRID_INTERFACE,
                                     STRID_MAC,
                                     EPNUM_NET_NOTIF,
                                     64,
                                     EPNUM_NET_OUT,
                                     EPNUM_NET_IN,
                                     CFG_TUD_NET_ENDPOINT_SIZE,
                                     CFG_TUD_NET_MTU,
                                     NCM_NETWORK_CAPS_IOS26),
};

const uint8_t* tud_descriptor_configuration_cb(uint8_t index)
{
    return index == 0 ? desc_configuration : NULL;
}

#define MS_OS_20_DESC_LEN 0xB2
#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1),
};

const uint8_t* tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

static const uint8_t desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_NCM, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'N', 'C', 'M', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007),
    U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0,
    't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0,
    'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '6', 0, 'D', 0, 'A', 0, 'C', 0, 'E', 0, '1', 0, '6', 0,
    '0', 0, '-', 0, '7', 0, '4', 0, '5', 0, 'B', 0, '-', 0, '4', 0,
    '7', 0, 'D', 0, '8', 0, '-', 0, '9', 0, 'A', 0, '3', 0, 'D', 0,
    '-', 0, '0', 0, '5', 0, 'D', 0, 'F', 0, '4', 0, '7', 0, 'B', 0,
    '9', 0, '8', 0, '0', 0, 'E', 0, '1', 0, '}', 0, 0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN,
                 "invalid Microsoft OS 2.0 descriptor length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t* request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR
        && request->bRequest == 1 && request->wIndex == 7) {
        return tud_control_xfer(rhport, request,
                                (void*)(uintptr_t)desc_ms_os_20,
                                sizeof(desc_ms_os_20));
    }
    return false;
}

extern uint8_t tud_network_mac_address[6];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t descriptor[33];
    static const char* const strings[] = {
        NULL, "Sipeed Co. Ltd.", "Sipeed T160 Thermal", NULL,
        "Sipeed Thermal USB Network",
    };
    unsigned count = 0;

    if (index == STRID_LANGID) {
        descriptor[1] = 0x0409;
        count = 1;
    } else if (index == STRID_MAC) {
        for (unsigned i = 0; i < sizeof(tud_network_mac_address); i++) {
            descriptor[1 + count++] =
                "0123456789ABCDEF"[tud_network_mac_address[i] >> 4];
            descriptor[1 + count++] =
                "0123456789ABCDEF"[tud_network_mac_address[i] & 0x0F];
        }
    } else {
        if (index >= sizeof(strings) / sizeof(strings[0])) return NULL;
        const char* string = index == STRID_SERIAL ? uid : strings[index];
        if (!string) return NULL;
        count = strlen(string);
        if (count > 32) count = 32;
        for (unsigned i = 0; i < count; i++) descriptor[1 + i] = string[i];
    }

    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (count * 2 + 2));
    return descriptor;
}
