/**
 * @file uac2_desc.c
 * @brief USB Audio Class 2.0 Descriptors for 192kHz/24bit Stereo DAC
 *
 * Phase 1: Enumeration-focused. Provides device / qualifier /
 * configuration / string descriptors via uac2_mk* helpers so that
 * uac2_driver.c can answer standard GET_DESCRIPTOR requests exactly
 * like cdcacm/usbmsc reference drivers.
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/audio.h>
#include "uac2_desc.h"

/* Standard Device Descriptor (USB 2.00, Misc/IAD class) */
const struct usb_devdesc_s g_uac2_device_desc = {
    USB_SIZEOF_DEVDESC,       /* len */
    USB_DESC_TYPE_DEVICE,     /* type */
    {
      LSBYTE(0x0200),
      MSBYTE(0x0200)
    },                        /* usb (BCD 2.00) */
    USB_CLASS_MISC,           /* classid */
    0x02,                     /* subclass: Common Class */
    0x01,                     /* protocol: IAD */
    UAC2_EP0MAXPACKET,        /* mxpacketsize (EP0) */
    {
      LSBYTE(UAC2_VENDOR_ID),
      MSBYTE(UAC2_VENDOR_ID)
    },                        /* vendor */
    {
      LSBYTE(UAC2_PRODUCT_ID),
      MSBYTE(UAC2_PRODUCT_ID)
    },                        /* product */
    {
      LSBYTE(UAC2_DEVICE_RELEASE_NUM),
      MSBYTE(UAC2_DEVICE_RELEASE_NUM)
    },                        /* device */
    UAC2_STR_MANUFACTURER,    /* imfgr */
    UAC2_STR_PRODUCT,         /* iproduct */
    UAC2_STR_SERIAL,          /* serno */
    UAC2_CONFIG_NCONFIGS      /* nconfigs */
};

#ifdef CONFIG_USBDEV_DUALSPEED
static const struct usb_qualdesc_s g_uac2_qualdesc =
{
    USB_SIZEOF_QUALDESC,      /* len */
    USB_DESC_TYPE_DEVICEQUALIFIER,
    {
      LSBYTE(0x0200),
      MSBYTE(0x0200)
    },
    USB_CLASS_MISC,
    0x02,
    0x01,
    UAC2_EP0MAXPACKET,
    UAC2_CONFIG_NCONFIGS,
    0
};
#endif

#define UAC2_TEST_PURE_STANDARD_DESC 0

#if UAC2_TEST_PURE_STANDARD_DESC
/* Diagnostic: Pure USB 2.0 standard descriptors only (no CS_INTERFACE / CS_ENDPOINT).
 * Used to verify whether CXD5602 USB IP hardware descriptor snooper chokes on UAC2 CS descriptors.
 * Total length: 9 (cfg) + 9 (if0 alt0) + 9 (if1 alt0) + 9 (if1 alt1) + 7 (ep2) + 9 (if1 alt2) + 7 (ep2) = 59 bytes.
 */
const uint8_t g_uac2_config_desc_hs[] = {
    /* 1. Configuration Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIG,               /* bDescriptorType */
    59, 0x00,                           /* wTotalLength: 59 bytes */
    0x02,                               /* bNumInterfaces: 2 */
    UAC2_CONFIG_ID,                     /* bConfigurationValue: 1 */
    0x00,                               /* iConfiguration */
    0xC0,                               /* bmAttributes: Self-powered */
    0x32,                               /* bMaxPower: 100mA */

    /* 2. Interface 0 (Alt 0) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    0x00,                               /* bInterfaceNumber: 0 */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 */
    0x01,                               /* bInterfaceClass: Audio (0x01) */
    0x01,                               /* bInterfaceSubClass: AudioControl (0x01) */
    0x20,                               /* bInterfaceProtocol: 0x20 */
    0x00,                               /* iInterface */

    /* 3. Interface 1 (Alt 0) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    0x01,                               /* bInterfaceNumber: 1 */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 */
    0x01,                               /* bInterfaceClass: Audio (0x01) */
    0x02,                               /* bInterfaceSubClass: AudioStreaming (0x02) */
    0x20,                               /* bInterfaceProtocol: 0x20 */
    0x00,                               /* iInterface */

    /* 4. Interface 1 (Alt 1) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    0x01,                               /* bInterfaceNumber: 1 */
    0x01,                               /* bAlternateSetting: 1 */
    0x01,                               /* bNumEndpoints: 1 */
    0x01,                               /* bInterfaceClass: Audio (0x01) */
    0x02,                               /* bInterfaceSubClass: AudioStreaming (0x02) */
    0x20,                               /* bInterfaceProtocol: 0x20 */
    0x00,                               /* iInterface */

    /* 5. Endpoint 2 OUT (Alt 1) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    0x02,                               /* bEndpointAddress: EP2 OUT */
    0x05,                               /* bmAttributes: Isochronous */
    100, 0x00,                          /* wMaxPacketSize: 100 bytes */
    0x01,                               /* bInterval: 1 */

    /* 6. Interface 1 (Alt 2) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    0x01,                               /* bInterfaceNumber: 1 */
    0x02,                               /* bAlternateSetting: 2 */
    0x01,                               /* bNumEndpoints: 1 */
    0x01,                               /* bInterfaceClass: Audio (0x01) */
    0x02,                               /* bInterfaceSubClass: AudioStreaming (0x02) */
    0x20,                               /* bInterfaceProtocol: 0x20 */
    0x00,                               /* iInterface */

    /* 7. Endpoint 2 OUT (Alt 2) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    0x02,                               /* bEndpointAddress: EP2 OUT */
    0x05,                               /* bmAttributes: Isochronous */
    200, 0x00,                          /* wMaxPacketSize: 200 bytes */
    0x01                                /* bInterval: 1 */
};
#else
/* High-Speed UAC2 Full Configuration Descriptor.
 * Totals: 202 bytes (0xCA) async with FU; 195 bytes (0xC3) adaptive
 * (feedback descriptor removed); 178 bytes (0xB2) Rev18 diagnostic
 * (Alt1 zero-bandwidth, Alt1 EP descs removed).
 * mkcfgdesc() patches wTotalLength from
 * sizeof() at runtime, but the static bytes must match for verification.
 */
const uint8_t g_uac2_config_desc_hs[] = {
    /* Configuration Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIG,               /* bDescriptorType */
#if UAC2_SINGLE_ALT0_STREAMING
    0x8A, 0x00,                         /* wTotalLength: 138 bytes (Alt0 single 24-bit PCM streaming) */
#elif UAC2_DIAG_ALT1_ZEROBW
    0xB2, 0x00,                         /* wTotalLength: 178 bytes (Alt1 zero-BW diag) */
#elif UAC2_SYNC_ADAPTIVE
    0xC3, 0x00,                         /* wTotalLength: 195 bytes (adaptive, no FB EP) */
#else
    0xCA, 0x00,                         /* wTotalLength: 202 bytes (async + FB EP) */
#endif
    UAC2_NUM_INTERFACES,                /* bNumInterfaces: 2 (AC + AS) */
    UAC2_CONFIG_ID,                     /* bConfigurationValue: 1 */
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
    0x01,                               /* bCategory: Desktop Speaker (0x01) */
    0x40, 0x00,                         /* wTotalLength: 64 bytes of AC descriptors (with FU) */
    0x00,                               /* bmControls: Latency Control not supported */

    /* Clock Source Descriptor (Entity ID 4) */
    0x08,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_CLOCK_SOURCE,               /* bDescriptorSubtype: CLOCK_SOURCE (0x0A) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bClockID: 4 */
    0x07,                               /* bmAttributes: Int.Prog.Clock + SOF sync (adaptive requires SOF lock) */
    0x03,                               /* bmControls: freq R/W (0x03) */
    0x00,                               /* bAssocTerminal: 0 (no association) */
    0x00,                               /* iClockSource */

    /* Input Terminal Descriptor (USB Streaming) (Entity ID 1) */
    0x11,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_INPUT_TERMINAL,             /* bDescriptorSubtype: INPUT_TERMINAL (0x02) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalID: 1 */
    (uint8_t)(UAC2_TERMINAL_STREAMING & 0xFF),
    (uint8_t)(UAC2_TERMINAL_STREAMING >> 8), /* wTerminalType: USB Streaming (0x0101) */
    0x00,                               /* bAssocTerminal */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bCSourceID: 4 (clocked by Entity 4) */
    0x02,                               /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: Front Left (0x01) | Front Right (0x02) */
    0x00,                               /* iChannelNames */
    0x00, 0x00,                         /* bmControls: None */
    0x00,                               /* iTerminal */

    /* Feature Unit Descriptor (Volume & Mute) (Entity ID 2) */
    0x12,                               /* bLength: 6 + (ch+1)*4 = 18 bytes */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_FEATURE_UNIT,               /* bDescriptorSubtype: FEATURE_UNIT (0x06) */
    UAC2_ENTITY_FEATURE_UNIT,           /* bUnitID: 2 */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bSourceID: 1 (from Input Terminal) */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(0): Master Mute R/W + Volume R/W */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(1): Ch1 Mute R/W + Volume R/W */
    0x0F, 0x00, 0x00, 0x00,             /* bmaControls(2): Ch2 Mute R/W + Volume R/W */
    0x00,                               /* iFeature */

    /* Output Terminal Descriptor (Desktop Speaker) (Entity ID 3) */
    0x0C,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_OUTPUT_TERMINAL,            /* bDescriptorSubtype: OUTPUT_TERMINAL (0x03) */
    UAC2_ENTITY_OUTPUT_TERMINAL,        /* bTerminalID: 3 */
    0x04, 0x03,                         /* wTerminalType: Desktop Speaker (0x0304) */
    0x00,                               /* bAssocTerminal: 0 (no association) */
    UAC2_ENTITY_FEATURE_UNIT,           /* bSourceID: 2 (from Feature Unit) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bCSourceID: 4 (clocked by Entity 4) */
    0x00, 0x00,                         /* bmControls: None */
    0x00,                               /* iTerminal */


#if UAC2_SINGLE_ALT0_STREAMING
    /* ========================================================================= */
    /* Interface 1: AudioStreaming (AS) - Single Alt 0 (24-bit Hi-Res Streaming) */
    /* ========================================================================= */
    /* Standard AS Interface Descriptor (Alt 0: Direct 24-bit Streaming) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x00,                               /* bAlternateSetting: 0 */
    0x01,                               /* bNumEndpoints: 1 (EP2 OUT) */
    ADC_CLASS,                          /* bInterfaceClass: Audio (0x01) */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* Class-Specific AS Interface General Descriptor */
    0x10,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_GENERAL,                    /* bDescriptorSubtype: AS_GENERAL (0x01) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalLink: 1 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    0x01, 0x00, 0x00, 0x00,             /* bmFormats: PCM (0x00000001) */
    UAC2_CHANNELS,                      /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: Front Left (0x01) | Front Right (0x02) */
    0x00,                               /* iChannelNames */

    /* Type I Format Type Descriptor (24-bit PCM in 32-bit slot) */
    0x06,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_FORMAT_TYPE,                /* bDescriptorSubtype: FORMAT_TYPE (0x02) */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    UAC2_SUBSLOT_SIZE_24,               /* bSubslotSize: 4 bytes (32-bit slot) */
    UAC2_BIT_DEPTH_24,                  /* bBitResolution: 24 bits */

    /* Standard AS Audio Data Endpoint Descriptor (EP2 OUT) */
    0x09,                               /* bLength: 9 (HS audio EP) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT (0x02) */
    0x09,                               /* bmAttributes: Isochronous, Adaptive (0x09) */
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K >> 8), /* wMaxPacketSize: 200 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 */
    0x00,                               /* bSynchAddress: 0 */

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00                          /* wLockDelay: 1 ms */
#else
    /* ========================================================================= */
    /* Interface 1: AudioStreaming (AS)                                         */
    /* ========================================================================= */
    /* Standard AS Interface Descriptor (Alt 0: Zero Bandwidth / Standby) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 */
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* ------------------------------------------------------------------------- */
    /* Alt Setting 1: 16-bit PCM Stereo (compat: MME/shared defaults)         */
    /* ------------------------------------------------------------------------- */
    /* Standard AS Interface Descriptor (Alt 1) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x01,                               /* bAlternateSetting: 1 */
#if UAC2_DIAG_ALT1_ZEROBW
    0x00,                               /* bNumEndpoints: 0 (Rev18 diag: Alt1 zero-BW) */
#elif UAC2_SYNC_ADAPTIVE
    0x01,                               /* bNumEndpoints: 1 (ISO OUT only) */
#else
    0x02,                               /* bNumEndpoints: 2 (ISO OUT + Feedback IN) */
#endif
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* Class-Specific AS Interface General Descriptor */
    0x10,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_GENERAL,                    /* bDescriptorSubtype: AS_GENERAL (0x01) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalLink: 1 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    0x01, 0x00, 0x00, 0x00,             /* bmFormats: PCM (0x00000001) */
    UAC2_CHANNELS,                      /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: Front Left (0x01) | Front Right (0x02) */
    0x00,                               /* iChannelNames */

    /* Type I Format Type Descriptor (16-bit PCM) */
    0x06,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_FORMAT_TYPE,                /* bDescriptorSubtype: FORMAT_TYPE (0x02) */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    UAC2_SUBSLOT_SIZE_16,               /* bSubslotSize: 2 bytes */
    UAC2_BIT_DEPTH_16,                  /* bBitResolution: 16 bits */

#if UAC2_DIAG_ALT1_ZEROBW
    /* Rev18 diag: Alt1 endpoint descriptors removed (zero-BW Alt1).
     * Alt1 keeps AS General + Format Type only.
     */
#else
#if UAC2_SYNC_ADAPTIVE
    0x09,                               /* bLength: 9 (HS audio EP: +bRefresh+bSynchAddress) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT */
    0x09,                               /* bmAttributes: Isochronous, Adaptive (0x09) */
    (uint8_t)(UAC2_PACKET_SIZE_16BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_16BIT_192K >> 8), /* wMaxPacketSize: 100 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 (data EP, no feedback rate) */
    0x00,                               /* bSynchAddress: 0 (adaptive: no sync EP) */
#else
    0x09,                               /* bLength: 9 (HS audio EP: +bRefresh+bSynchAddress) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT */
    0x05,                               /* bmAttributes: Isochronous, Asynchronous (0x05) */
    (uint8_t)(UAC2_PACKET_SIZE_16BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_16BIT_192K >> 8), /* wMaxPacketSize: 100 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 (data EP, no feedback rate) */
    UAC2_EP_FEEDBACK_IN,                /* bSynchAddress: EP1 IN (explicit feedback) */
#endif

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00,                         /* wLockDelay: 1 ms */
#endif /* !UAC2_DIAG_ALT1_ZEROBW */
    /* ------------------------------------------------------------------------- */
    /* Alt Setting 2: 24-bit PCM (in 32-bit slot) Hi-Res Audio Streaming        */
    /* ------------------------------------------------------------------------- */
    /* Standard AS Interface Descriptor (Alt 2) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x02,                               /* bAlternateSetting: 2 */
#if UAC2_SYNC_ADAPTIVE
    0x01,                               /* bNumEndpoints: 1 (ISO OUT only) */
#else
    0x02,                               /* bNumEndpoints: 2 (ISO OUT + Feedback IN) */
#endif
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* Class-Specific AS Interface General Descriptor */
    0x10,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_GENERAL,                    /* bDescriptorSubtype: AS_GENERAL (0x01) */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bTerminalLink: 1 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    0x01, 0x00, 0x00, 0x00,             /* bmFormats: PCM (0x00000001) */
    UAC2_CHANNELS,                      /* bNrChannels: 2 */
    0x03, 0x00, 0x00, 0x00,             /* bmChannelConfig: Front Left (0x01) | Front Right (0x02) */
    0x00,                               /* iChannelNames */

    /* Type I Format Type Descriptor (24-bit PCM in 32-bit slot) */
    0x06,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AS_FORMAT_TYPE,                /* bDescriptorSubtype: FORMAT_TYPE (0x02) */
    0x01,                               /* bFormatType: FORMAT_TYPE_I (0x01) */
    UAC2_SUBSLOT_SIZE_24,               /* bSubslotSize: 4 bytes (32-bit slot) */
    UAC2_BIT_DEPTH_24,                  /* bBitResolution: 24 bits */

#if UAC2_SYNC_ADAPTIVE
    0x09,                               /* bLength: 9 (HS audio EP: +bRefresh+bSynchAddress) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT */
    0x09,                               /* bmAttributes: Isochronous, Adaptive (0x09) */
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K >> 8), /* wMaxPacketSize: 200 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 (data EP, no feedback rate) */
    0x00,                               /* bSynchAddress: 0 (adaptive: no sync EP) */
#else
    0x09,                               /* bLength: 9 (HS audio EP: +bRefresh+bSynchAddress) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT */
    0x05,                               /* bmAttributes: Isochronous, Asynchronous (0x05) */
#if UAC2_WMAX_NOMINAL
    0xC0, 0x00,                         /* wMaxPacketSize: 192 bytes (exact nominal) */
#else
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K >> 8), /* wMaxPacketSize: 200 bytes */
#endif
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 (data EP, no feedback rate) */
    UAC2_EP_FEEDBACK_IN,                /* bSynchAddress: EP1 IN (explicit feedback) */
#endif

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00,                         /* wLockDelay: 1 ms */

#if !UAC2_SYNC_ADAPTIVE
    /* Standard AS Feedback Endpoint Descriptor (IN) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_FEEDBACK_IN,                /* bEndpointAddress: EP1 IN */
    0x11,                               /* bmAttributes: Isochronous, Feedback (0x11) */
    0x04, 0x00,                         /* wMaxPacketSize: 4 bytes */
    0x01                                /* bInterval: 1 (1 microframe = 125us) */
#endif /* !UAC2_SYNC_ADAPTIVE */
#endif
};
#endif /* !UAC2_TEST_PURE_STANDARD_DESC */


const uint16_t g_uac2_config_desc_hs_len = sizeof(g_uac2_config_desc_hs);

/* Descriptor accessors */

FAR const struct usb_devdesc_s *uac2_getdevdesc(void)
{
    return &g_uac2_device_desc;
}

#ifdef CONFIG_USBDEV_DUALSPEED
FAR const struct usb_qualdesc_s *uac2_getqualdesc(void)
{
    return &g_uac2_qualdesc;
}
#endif

#ifdef CONFIG_USBDEV_DUALSPEED
int16_t uac2_mkcfgdesc(FAR uint8_t *buf, uint8_t speed, uint8_t type)
#else
int16_t uac2_mkcfgdesc(FAR uint8_t *buf)
#endif
{
#ifdef CONFIG_USBDEV_DUALSPEED
    uint8_t desttype = type;
    (void)speed;
#else
    uint8_t desttype = USB_DESC_TYPE_CONFIG;
#endif

    if (buf == NULL)
      {
        return (int16_t)sizeof(g_uac2_config_desc_hs);
      }

    memcpy(buf, g_uac2_config_desc_hs, sizeof(g_uac2_config_desc_hs));

    /* Patch descriptor type (CONFIG vs OTHERSPEEDCONFIG) and total length.
     * Static table already holds correct wTotalLength (0xC3/0xCA), but patch
     * defensively so future edits cannot desync.
     */

    buf[1] = desttype;
    buf[2] = LSBYTE(sizeof(g_uac2_config_desc_hs));
    buf[3] = MSBYTE(sizeof(g_uac2_config_desc_hs));

    return (int16_t)sizeof(g_uac2_config_desc_hs);
}

/* String descriptors */

int uac2_mkstrdesc(uint8_t id, FAR struct usb_strdesc_s *strdesc)
{
    FAR uint8_t *data = (FAR uint8_t *)(strdesc + 1);
    FAR const char *str;
    int len;
    int ndata;
    int i;

    switch (id)
      {
      case 0:
        {
          strdesc->len  = 4;
          strdesc->type = USB_DESC_TYPE_STRING;
          data[0] = LSBYTE(UAC2_STR_LANGUAGE);
          data[1] = MSBYTE(UAC2_STR_LANGUAGE);
          return 4;
        }

      case UAC2_STR_MANUFACTURER:
        str = "Sony";
        break;

      case UAC2_STR_PRODUCT:
        str = "Spresense 192kHz/24bit Audio";
        break;

      case UAC2_STR_SERIAL:
        str = "0005"; /* Rev20: new serial forces a fresh Windows device instance */
        break;

      case UAC2_STR_CONFIG:
        str = "UAC2 Config";
        break;

      case UAC2_STR_CLOCK_SOURCE:
        str = "Internal Clock";
        break;

      default:
        return -EINVAL;
      }

    /* Poor-man's UTF-8 -> UTF-16LE (ASCII only, same as cdcacm) */

    len = strlen(str);
    if (len > ((UAC2_MXDESCLEN - 2) / 2))
      {
        len = (UAC2_MXDESCLEN - 2) / 2;
      }

    for (i = 0, ndata = 0; i < len; i++, ndata += 2)
      {
        data[ndata]     = str[i];
        data[ndata + 1] = 0;
      }

    strdesc->len  = ndata + 2;
    strdesc->type = USB_DESC_TYPE_STRING;
    return strdesc->len;
}
