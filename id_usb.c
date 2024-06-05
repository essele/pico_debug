
/**
 * @file usb.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-02-25
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <stdarg.h>
#include "tusb.h"
//#include "hardware/irq.h"
#include "pico/unique_id.h"


//#define USBD_VID (0x2E8A) // Raspberry Pi
//#define USBD_PID (0x000a) // Raspberry Pi Pico SDK CDC

#define USBD_VID (0x3333) // Dummy Testing Values
#define USBD_PID (0x1111) // Dummy Testing Values

#define USB_IRQ 31
#define USB_WORKER_INTERVAL_US  1000
#define USB_WRITE_TIMEOUT_US    500000

#define USBD_CDC_0_EP_CMD (0x81)
#define USBD_CDC_0_EP_OUT (0x02)
#define USBD_CDC_0_EP_IN (0x82)

#define USBD_CDC_1_EP_CMD (0x83)
#define USBD_CDC_1_EP_OUT (0x03)
#define USBD_CDC_1_EP_IN (0x84)

#define USBD_CDC_2_EP_CMD (0x85)
#define USBD_CDC_2_EP_OUT (0x04)
#define USBD_CDC_2_EP_IN (0x86)

#define USBD_CDC_3_EP_CMD (0x87)
#define USBD_CDC_3_EP_OUT (0x08)
#define USBD_CDC_3_EP_IN (0x89)

#define USBD_CDC_CMD_MAX_SIZE (8)
#define USBD_CDC_IN_OUT_MAX_SIZE (64)

#define USBD_STR_0 (0x00)
#define USBD_STR_MANUF (0x01)
#define USBD_STR_PRODUCT (0x02)
#define USBD_STR_SERIAL (0x03)
#define USBD_STR_CDC0 (0x04)
#define USBD_STR_CDC1 (0x05)
#define USBD_STR_CDC2 (0x06)
#define USBD_STR_CDC3 (0x07)

//
// Officially, according to the USB specs, we shoul duse TUSB_CLASS_MISC, SUBCLASS_COMMON, and PROTOCOL_IAD
// so that we can "group" the two CDC interfaces together, however if you do that then the NI tools don't
// seem to work properly!
//
// Breaking the standard, and sticking with 0, 0, 0 seems to work for both Linux and Windows, ultimately if
// this becomes a problem then it could be configurable since the CDC interface works fine in both cases.
//


static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};


enum
{
  ITF_NUM_CDC_0 = 0,
  ITF_NUM_CDC_0_DATA,
  ITF_NUM_CDC_1,
  ITF_NUM_CDC_1_DATA,
  ITF_NUM_CDC_2,
  ITF_NUM_CDC_2_DATA,
  ITF_NUM_CDC_3,
  ITF_NUM_CDC_3_DATA,
  ITF_NUM_TOTAL
};


#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + (3 * TUD_CDC_DESC_LEN))

uint8_t const usbd_desc_cfg[] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0, USBD_STR_CDC0, USBD_CDC_0_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_0_EP_OUT, USBD_CDC_0_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_1, USBD_STR_CDC1, USBD_CDC_1_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_1_EP_OUT, USBD_CDC_1_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_2, USBD_STR_CDC2, USBD_CDC_2_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_2_EP_OUT, USBD_CDC_2_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_3, USBD_STR_CDC3, USBD_CDC_3_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_3_EP_OUT, USBD_CDC_3_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),
};


const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(__unused uint8_t index) {
    return usbd_desc_cfg;
}

/**
 * @brief Convert a normal c string into a USB string descriptor
 * 
 * @param str 
 * @param desc_str 
 * @return uint8_t 
 */
static uint8_t string_to_descriptor(char *str, uint16_t *desc_str) {
    #define DESC_STR_MAX (32)

    int len;

    for (len = 0; len < DESC_STR_MAX - 1 && str[len]; ++len) {
        desc_str[1 + len] = str[len];
    }
    return len;
}

/**
 * @brief USB Callback to provide descriptor strings (from our config)
 * 
 * @param index 
 * @param langid 
 * @return const uint16_t* 
 */
const uint16_t *tud_descriptor_string_cb(uint8_t index, __unused uint16_t langid) {
    static uint16_t desc_str[DESC_STR_MAX];

    uint8_t len;
    switch (index) {
        case 0:     
            desc_str[1] = 0x0409;       // supported language is English
            len = 1;
            break;

        case USBD_STR_MANUF:
            len = string_to_descriptor("manuf", desc_str);
            break;

        case USBD_STR_PRODUCT:
            len = string_to_descriptor("product", desc_str);
            break;

        case USBD_STR_SERIAL:
            len = string_to_descriptor("pdebug", desc_str);
            break;

        case USBD_STR_CDC0:
            len = string_to_descriptor("debug-gdb", desc_str);
            break;

        case USBD_STR_CDC1:
            len = string_to_descriptor("debug-uart", desc_str);
            break;

        case USBD_STR_CDC2:
            len = string_to_descriptor("debug-debug", desc_str);
            break;

        case USBD_STR_CDC3:
            len = string_to_descriptor("debug-prof", desc_str);
            break;

        default:
            return NULL;
    }
    // first byte is length (including header), second byte is string type
    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * len + 2));

    return desc_str;
}
