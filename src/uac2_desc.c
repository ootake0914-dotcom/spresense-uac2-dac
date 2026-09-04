/**
 * @file uac2_desc.c
 * @brief USB Audio Class 2.0 Descriptors for 192kHz/24bit Stereo DAC
 */

#include <stdint.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/audio.h>
#include "uac2_desc.h"

/* Standard Device Descriptor */
const struct usb_devdesc_s g_uac2_device_desc = {
    .len         = USB_SIZEOF_DEVDESC,
    .type        = USB_DESC_TYPE_DEVICE,
    .bcdUSB      = { 0x00, 0x02 }, /* USB 2.00 */
    .classid     = USB_CLASS_MISC,
    .subclass    = 0x02,           /* Common Class */
    .protocol    = 0x01,           /* Interface Association Descriptor */
    .mxpacketsize= 64,             /* EP0 max packet size */
    .vendor      = { (uint8_t)(UAC2_VENDOR_ID & 0xFF), (uint8_t)(UAC2_VENDOR_ID >> 8) },
    .product     = { (uint8_t)(UAC2_PRODUCT_ID & 0xFF), (uint8_t)(UAC2_PRODUCT_ID >> 8) },
    .device      = { (uint8_t)(UAC2_DEVICE_RELEASE_NUM & 0xFF), (uint8_t)(UAC2_DEVICE_RELEASE_NUM >> 8) },
    .imfc        = UAC2_STR_MANUFACTURER,
    .iproduct    = UAC2_STR_PRODUCT,
    .serno       = UAC2_STR_SERIAL,
    .nconfigs    = 1
};

/* High-Speed UAC2 Full Configuration Descriptor */
const uint8_t g_uac2_config_desc_hs[] = {
    /* Configuration Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIG,               /* bDescriptorType */
    0x00, 0x00,                         /* wTotalLength (placeholder, set dynamically or below) */
    UAC2_NUM_INTERFACES,                /* bNumInterfaces: 2 (AC + AS) */
    0x01,                               /* bConfigurationValue: 1 */
    UAC2_STR_CONFIG,                    /* iConfiguration */
    0xC0,                               /* bmAttributes: Self-powered */
    0x32,                               /* bMaxPower: 100mA (50 * 2mA) */

    /* Interface Association Descriptor (IAD) */
    0x08,                               /* bLength */
    USB_DESC_TYPE_INTERFACEASSOCIATION, /* bDescriptorType */
    UAC2_IF_AUDIO_CONTROL,              /* bFirstInterface */
    UAC2_NUM_INTERFACES,                /* bInterfaceCount */
    ADC_CLASS,                          /* bFunctionClass: Audio (0x01) */
    UAC2_SUBCLASS_UNDEFINED,            /* bFunctionSubClass */
    ADC_PROTOCOLv20,                    /* bFunctionProtocol: IP version 2.0 (0x20) */
    UAC2_STR_PRODUCT,                   /* iFunction */

    /* ========================================================================= */
    /* Interface 0: AudioControl (AC)                                           */
    /* ========================================================================= */
    /* Standard AC Interface Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_CONTROL,              /* bInterfaceNumber */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 (uses EP0 control) */
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOCONTROL,         /* bInterfaceSubClass: AudioControl (0x01) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    UAC2_STR_PRODUCT,                   /* iInterface */

    /* Class-Specific AC Interface Header Descriptor */
    0x09,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_HEADER,                     /* bDescriptorSubtype: HEADER (0x01) */
    0x00, 0x02,                         /* bcdADC: Audio Class 2.0 (0x0200) */
    0x01,                               /* bCategory: Desktop Speaker / DAC (0x01) */
    0x3C, 0x00,                         /* wTotalLength: 60 bytes of AC descriptors */
    0x00,                               /* bmControls: Latency Control not supported */

    /* Clock Source Descriptor (Entity ID 1) */
    0x08,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_CLOCK_SOURCE,               /* bDescriptorSubtype: CLOCK_SOURCE (0x0A) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bClockID: 1 */
    UAC2_CLOCK_SOURCE_INT_PROG,         /* bmAttributes: Internal Programmable Clock */
    0x07,                               /* bmControls: freq read/write, validity read */
    0x00,                               /* bAssocTerminal */
    UAC2_STR_CLOCK_SOURCE,              /* iClockSource */

    /* Input Terminal Descriptor (USB Streaming) (Entity ID 2) */
    0x11,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_INPUT_TERMINAL,             /* bDescriptorSubtype: INPUT_TERMINAL (0x02) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalID: 2 */
    (uint8_t)(UAC2_TERMINAL_STREAMING & 0xFF),
    (uint8_t)(UAC2_TERMINAL_STREAMING >> 8), /* wTerminalType: USB Streaming (0x0101) */
    0x00,                               /* bAssocTerminal */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bCSourceID: 1 (clocked by Entity 1) */
    0x02,                               /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: Front Left + Front Right */
    0x00,                               /* iChannelNames */
    0x00, 0x00,                         /* bmControls: None */
    0x00,                               /* iTerminal */

    /* Feature Unit Descriptor (Volume & Mute) (Entity ID 3) */
    0x12,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_FEATURE_UNIT,               /* bDescriptorSubtype: FEATURE_UNIT (0x06) */
    UAC2_ENTITY_FEATURE_UNIT,           /* bUnitID: 3 */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bSourceID: 2 (from Input Terminal) */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(0): Master Mute + Volume (R/W) */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(1): Ch1 Mute + Volume (R/W) */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(2): Ch2 Mute + Volume (R/W) */
    0x00,                               /* iFeature */

    /* Output Terminal Descriptor (Headphones) (Entity ID 4) */
    0x0C,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_OUTPUT_TERMINAL,            /* bDescriptorSubtype: OUTPUT_TERMINAL (0x03) */
    UAC2_ENTITY_OUTPUT_TERMINAL,        /* bTerminalID: 4 */
    (uint8_t)(UAC2_OUTPUT_TERMINAL_HEADPHONES & 0xFF),
    (uint8_t)(UAC2_OUTPUT_TERMINAL_HEADPHONES >> 8), /* wTerminalType: Headphones (0x0302) */
    0x00,                               /* bAssocTerminal */
    UAC2_ENTITY_FEATURE_UNIT,           /* bSourceID: 3 (from Feature Unit) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bCSourceID: 1 (clocked by Entity 1) */
    0x00, 0x00,                         /* bmControls: None */
    0x00,                               /* iTerminal */

    /* ========================================================================= */
    /* Interface 1: AudioStreaming (AS)                                         */
    /* ========================================================================= */
    /* Standard AS Interface Descriptor (Alt 0: Zero Bandwidth) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 */
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* Standard AS Interface Descriptor (Alt 1: Active 192kHz/24bit PCM) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x01,                               /* bAlternateSetting: 1 */
    0x02,                               /* bNumEndpoints: 2 (ISO OUT + Feedback IN) */
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* Class-Specific AS Interface General Descriptor */
    0x10,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_GENERAL,                    /* bDescriptorSubtype: AS_GENERAL (0x01) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalLink: 2 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    0x01, 0x00, 0x00, 0x00,             /* bmFormats: PCM (0x00000001) */
    UAC2_CHANNELS,                      /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: FL + FR */
    0x00,                               /* iChannelNames */

    /* Type I Format Type Descriptor */
    0x06,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_FORMAT_TYPE,                /* bDescriptorSubtype: FORMAT_TYPE (0x02) */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    UAC2_SUBSLOT_SIZE,                  /* bSubslotSize: 4 bytes (32-bit slot) */
    UAC2_BIT_DEPTH,                     /* bBitResolution: 24 bits */

    /* Standard AS Isochronous Audio Data Endpoint Descriptor (OUT) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP1 OUT */
    0x05,                               /* bmAttributes: Isochronous, Asynchronous (0x05) */
    (uint8_t)(UAC2_PACKET_SIZE_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_192K >> 8), /* wMaxPacketSize: 192 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x00,                               /* bLockDelayUnits */
    0x00, 0x00,                         /* wLockDelay */

    /* Standard AS Feedback Endpoint Descriptor (IN) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_FEEDBACK_IN,                /* bEndpointAddress: EP1 IN */
    0x11,                               /* bmAttributes: Isochronous, Feedback (0x11) */
    0x04, 0x00,                         /* wMaxPacketSize: 4 bytes (12.13 or 10.14 format) */
    0x04                                /* bInterval: 4 (every 2^(4-1) = 8 microframes = 1ms) */
};

const uint16_t g_uac2_config_desc_hs_len = sizeof(g_uac2_config_desc_hs);
