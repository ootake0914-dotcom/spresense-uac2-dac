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

/* Standard Device Descriptor (USB 2.00 for pure standard UAC2, Misc/IAD class) */
const struct usb_devdesc_s g_uac2_device_desc = {
    USB_SIZEOF_DEVDESC,       /* len */
    USB_DESC_TYPE_DEVICE,     /* type */
    {
      LSBYTE(0x0200),
      MSBYTE(0x0200)
    },                        /* usb (BCD 2.00: standard USB 2.0, no BOS query) */
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

/* BOS descriptor (33B): MS OS 2.0 platform capability only.
 * Lets Windows >= 8.1 auto-bind WinUSB (no INF/signing) for MI_01.
 * NOTE: the MS OS 2.0 Platform Capability descriptor is 28B and MUST
 * contain dwWindowsVersion (4B) + wMSOSDescriptorSetTotalLength (2B)
 * between the UUID and bVendorCode (the missing 6 bytes that made
 * hosts reject a truncated capability).
 */
const uint8_t g_uac2_bos_desc[] =
{
    0x05,                               /* bLength */
    USB_DESC_TYPE_BOS,                  /* bDescriptorType: BOS (0x0F) */
    0x21, 0x00,                         /* wTotalLength: 33 */
    0x01,                               /* bNumDeviceCaps: 1 */
    /* Microsoft OS 2.0 Platform Capability (28B) */
    0x1C,                               /* bLength */
    0x10,                               /* bDescriptorType: DEVICE_CAPABILITY */
    0x05,                               /* bDevCapabilityType: PLATFORM */
    0x00,                               /* bReserved */
    0xDF, 0x60, 0xDD, 0xD8,             /* MS OS 2.0 UUID */
    0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D,
    0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,             /* dwWindowsVersion: 8.1 */
    0x2E, 0x00,                         /* wMSOSDescriptorSetTotalLength: 46 */
    UAC2_MS_VENDOR_CODE,                /* bVendorCode */
    0x00                                /* bAltEnumCode */
};

_Static_assert(sizeof(g_uac2_bos_desc) == 33,
               "BOS descriptor must be 33 bytes");

/* MS OS 2.0 Descriptor Set (46B): MI_01 (AS streaming) -> WINUSB.
 * AC (MI_00) stays with usbaudio2 (fails as before, harmless).
 */
const uint8_t g_uac2_msos20_set[] =
{
    /* Set header (10B) */
    0x0A, 0x00,                         /* wLength */
    0x00, 0x00,                         /* wDescriptorType: SET_HEADER */
    0x00, 0x00, 0x03, 0x06,             /* dwWindowsVersion 8.1 */
    0x2E, 0x00,                         /* wTotalLength: 46 */
    /* Configuration subset (8B) */
    0x08, 0x00,                         /* wLength */
    0x01, 0x00,                         /* wDescriptorType: CONFIG_SUBSET */
    UAC2_CONFIG_ID,                     /* bConfigurationValue */
    0x00,                               /* bReserved */
    0x24, 0x00,                         /* wTotalLength: 36 */
    /* Function subset (8B): AS interface only */
    0x08, 0x00,                         /* wLength */
    0x02, 0x00,                         /* wDescriptorType: FUNCTION_SUBSET */
    UAC2_IF_AUDIO_STREAMING,            /* bFirstInterface */
    0x00,                               /* bReserved */
    0x1C, 0x00,                         /* wTotalLength: 28 */
    /* Compatible ID (20B): WINUSB */
    0x14, 0x00,                         /* wLength */
    0x03, 0x00,                         /* wDescriptorType: COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint16_t g_uac2_bos_desc_len = sizeof(g_uac2_bos_desc);
const uint16_t g_uac2_msos20_set_len = sizeof(g_uac2_msos20_set);

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
 * Totals: 202 bytes (0xCA) async with FU (multi-alt layout);
 *         195 bytes (0xC3) adaptive (feedback descriptor removed);
 *         178 bytes (0xB2) Rev18 diagnostic (Alt1 zero-bandwidth);
 *         145 bytes (0x91) Rev76 async Alt-0 single (138 + 7B FB EP).
 * mkcfgdesc() patches wTotalLength from
 * sizeof() at runtime, but the static bytes must match for verification.
 */
const uint8_t g_uac2_config_desc_hs[] = {
    /* Configuration Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIG,               /* bDescriptorType */
#if UAC2_SINGLE_ALT0_STREAMING
    0x91, 0x00,                         /* wTotalLength: 145 bytes (Rev76 async Alt0 single + FB EP) */
#else
    0xBF, 0x00,                         /* wTotalLength: 191 bytes (W13: Multi-Alt 48k Alt1 + 192k Alt2) */
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
    0x40, 0x00,                         /* wTotalLength: 64 bytes of AC descriptors (with Master-only FU) */
    0x00,                               /* bmControls: Latency Control not supported */

    /* Clock Source Descriptor (Entity ID 4): 48k/192k programmable internal clock. */
    0x08,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_CLOCK_SOURCE,               /* bDescriptorSubtype: CLOCK_SOURCE (0x0A) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bClockID: 4 */
    UAC2_CLOCK_SOURCE_INT_PROG,         /* bmAttributes: internal programmable clock (0x03) */
    0x07,                               /* bmControls: D1..0=11 (freq host programmable), D3..2=01 (valid read-only) */
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

    /* Feature Unit Descriptor (Volume & Mute) (Entity ID 2)
     * Master channel only: eliminates Windows usbaudio2.sys per-channel retry loops
     * while giving Windows the volume/mute node it needs to populate the audio endpoint.
     */
    0x12,                               /* bLength: 6 + (2+1)*4 = 18 bytes */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_FEATURE_UNIT,               /* bDescriptorSubtype: FEATURE_UNIT (0x06) */
    UAC2_ENTITY_FEATURE_UNIT,           /* bUnitID: 2 */
    UAC2_ENTITY_INPUT_TERMINAL,         /* bSourceID: 1 (from Input Terminal) */
    /* bmaControls(0) - Master: Mute R/W (0x03), Volume R/W (0x0C) -> 0x0F */
    0x0F, 0x00, 0x00, 0x00,
    /* bmaControls(1) - Front Left: None (0x00) -> single-channel bypass */
    0x00, 0x00, 0x00, 0x00,
    /* bmaControls(2) - Front Right: None (0x00) */
    0x00, 0x00, 0x00, 0x00,
    0x00,                               /* iFeature: 0 */

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
    0x02,                               /* bNumEndpoints: 2 (EP2 OUT + EP1 IN feedback) */
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
    0x05,                               /* bmAttributes: Isochronous, Asynchronous (0x05) */
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K >> 8), /* wMaxPacketSize: 200 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */
    0x00,                               /* bRefresh: 0 */
    UAC2_EP_FEEDBACK_IN,                /* bSynchAddress: EP1 IN (explicit feedback) */

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00,                         /* wLockDelay: 1 ms */

    /* Standard AS Feedback Endpoint Descriptor (EP1 IN, Rev76) */
    0x07,                               /* bLength */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType */
    UAC2_EP_FEEDBACK_IN,                /* bEndpointAddress: EP1 IN (0x81) */
    0x11,                               /* bmAttributes: Isochronous, Feedback (0x11) */
    0x04, 0x00,                         /* wMaxPacketSize: 4 bytes (Q16.16) */
    0x04                                /* bInterval: 4 (2^3 uframes = 1ms) */
#else
    /* ========================================================================= */
    /* Interface 1: AudioStreaming (AS) - W0/W1 Clean Multi-Alt (Alt0 + Alt1)    */
    /* ========================================================================= */
    /* Standard AS Interface Descriptor (Alt 0: Zero Bandwidth / Standby) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x00,                               /* bAlternateSetting: 0 */
    0x00,                               /* bNumEndpoints: 0 (Zero Bandwidth) */
    ADC_CLASS,                          /* bInterfaceClass: Audio */
    UAC2_SUBCLASS_AUDIOSTREAMING,       /* bInterfaceSubClass: AudioStreaming (0x02) */
    ADC_PROTOCOLv20,                    /* bInterfaceProtocol: IP version 2.0 (0x20) */
    0x00,                               /* iInterface */

    /* ------------------------------------------------------------------------- */
    /* Alt Setting 1: 48kHz / 24-bit PCM (in 32-bit slot) Adaptive Streaming     */
    /* ------------------------------------------------------------------------- */
    /* Standard AS Interface Descriptor (Alt 1) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x01,                               /* bAlternateSetting: 1 */
    0x01,                               /* bNumEndpoints: 1 (EP2 OUT only, Adaptive) */
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

    /* Standard AS Audio Data Endpoint Descriptor (EP2 OUT, UAC2 7-byte) */
    0x07,                               /* bLength: 7 (UAC2 Section 4.10.1.1 standard endpoint) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType: ENDPOINT (0x05) */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT (0x02) */
#if E3_BULK_ALT1
    0x02,                               /* E3: Bulk (Alt-1 gate probe) */
#else
    0x09,                               /* bmAttributes: Isochronous, Adaptive (0x09) */
#endif
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_48K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_48K >> 8), /* wMaxPacketSize: 56 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00,                         /* wLockDelay: 1 ms */

    /* ------------------------------------------------------------------------- */
    /* Alt Setting 2: 192kHz / 24-bit PCM (in 32-bit slot) Adaptive Streaming    */
    /* ------------------------------------------------------------------------- */
    /* Standard AS Interface Descriptor (Alt 2) */
    0x09,                               /* bLength */
    USB_DESC_TYPE_INTERFACE,            /* bDescriptorType */
    UAC2_IF_AUDIO_STREAMING,            /* bInterfaceNumber: 1 */
    0x02,                               /* bAlternateSetting: 2 */
    0x01,                               /* bNumEndpoints: 1 (EP2 OUT only, Adaptive) */
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

    /* Standard AS Audio Data Endpoint Descriptor (EP2 OUT, UAC2 7-byte) */
    0x07,                               /* bLength: 7 (UAC2 Section 4.10.1.1 standard endpoint) */
    USB_DESC_TYPE_ENDPOINT,             /* bDescriptorType: ENDPOINT (0x05) */
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT (0x02) */
    0x09,                               /* bmAttributes: Isochronous, Adaptive (0x09) */
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K & 0xFF),
    (uint8_t)(UAC2_PACKET_SIZE_24BIT_192K >> 8), /* wMaxPacketSize: 200 bytes */
    0x01,                               /* bInterval: 1 (1 microframe = 125us) */

    /* Class-Specific AS Audio Data Endpoint Descriptor */
    0x08,                               /* bLength */
    ADC_CS_ENDPOINT,                    /* bDescriptorType: CS_ENDPOINT (0x25) */
    UAC2_EP_GENERAL,                    /* bDescriptorSubtype: EP_GENERAL (0x01) */
    0x00,                               /* bmAttributes: MaxPacketsOnly=0 */
    0x00,                               /* bmControls: None */
    0x01,                               /* bLockDelayUnits: Milliseconds (1) */
    0x01, 0x00                          /* wLockDelay: 1 ms */
#endif
};
#endif /* !UAC2_TEST_PURE_STANDARD_DESC */

#if !UAC2_SINGLE_ALT0_STREAMING
_Static_assert(sizeof(g_uac2_config_desc_hs) == 191,
               "W13 Configuration descriptor length must be exactly 191 bytes (0x00BF)");
#endif


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
        str = "0018"; /* W13: Multi-Alt 48k/192k */
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
