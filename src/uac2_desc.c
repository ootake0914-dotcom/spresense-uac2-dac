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

/* High-Speed UAC2 Full Configuration Descriptor (total 152 bytes = 0x98) */
const uint8_t g_uac2_config_desc_hs[] = {
    /* Configuration Descriptor */
    0x09,                               /* bLength */
    USB_DESC_TYPE_CONFIG,               /* bDescriptorType */
    0x98, 0x00,                         /* wTotalLength: 152 bytes */
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
    0x40, 0x00,                         /* wTotalLength: 64 bytes of AC descriptors */
    0x00,                               /* bmControls: Latency Control not supported */

    /* Clock Source Descriptor (Entity ID 1) */
    0x08,                               /* bLength */
    ADC_CS_INTERFACE,                   /* bDescriptorType: CS_INTERFACE (0x24) */
    UAC2_AC_CLOCK_SOURCE,               /* bDescriptorSubtype: CLOCK_SOURCE (0x0A) */
    UAC2_ENTITY_CLOCK_SOURCE,           /* bClockID: 1 */
    UAC2_CLOCK_SOURCE_INT_PROG,         /* bmAttributes: Internal Programmable Clock */
    0x07,                               /* bmControls: freq R/W, validity R */
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
    UAC2_EP_ISO_OUT,                    /* bEndpointAddress: EP2 OUT */
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
    0x04, 0x00,                         /* wMaxPacketSize: 4 bytes */
    0x04                                /* bInterval: 4 (8 uframes = 1ms) */
};

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
     * Static table already holds correct wTotalLength (0x98), but patch
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
        str = "0001";
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
