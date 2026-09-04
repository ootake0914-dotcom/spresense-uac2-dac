/**
 * @file uac2_desc.h
 * @brief USB Audio Class 2.0 Descriptor Definitions for Spresense
 */

#ifndef __UAC2_DESC_H
#define __UAC2_DESC_H

#include <stdint.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include "uac2.h"

#define UAC2_VENDOR_ID                0x054C /* Sony Corporation */
#define UAC2_PRODUCT_ID               0x0CE6 /* Spresense UAC2 Hi-Res DAC */
#define UAC2_DEVICE_RELEASE_NUM       0x0100 /* v1.00 */

/* Entity IDs */
#define UAC2_ENTITY_CLOCK_SOURCE      0x01
#define UAC2_ENTITY_INPUT_TERMINAL    0x02
#define UAC2_ENTITY_FEATURE_UNIT      0x03
#define UAC2_ENTITY_OUTPUT_TERMINAL   0x04

/* Interface Numbers */
#define UAC2_IF_AUDIO_CONTROL         0x00
#define UAC2_IF_AUDIO_STREAMING       0x01
#define UAC2_NUM_INTERFACES           0x02

/* Configuration */
#define UAC2_CONFIG_ID                0x01
#define UAC2_CONFIG_NCONFIGS          0x01
#define UAC2_MXDESCLEN                256

/* Endpoint Addresses
 * NOTE (CXD5602 constraint): cxd56_usbdev.c allocates endpoints by
 * EP number + direction + type, with fixed HW mapping:
 *   EP1/3/4/6 = IN only, EP2/5 = OUT only.
 * Sharing EP number 1 for OUT+IN (typical UAC2 0x01/0x81) can never
 * be allocated on this DCD (second alloc finds no free EP with the
 * same number). So we use distinct numbers that match the existing
 * BULK EP numbers to minimise the Phase-2 ISOC DCD patch:
 *   ISO OUT = EP2 OUT (0x02), Feedback IN = EP1 IN (0x81).
 */
#define UAC2_EP_ISO_OUT               0x02 /* OUT Endpoint 2 (audio data) */
#define UAC2_EP_ISO_OUT_NUM           0x02
#define UAC2_EP_FEEDBACK_IN           0x81 /* IN Endpoint 1 (feedback) */
#define UAC2_EP_FEEDBACK_IN_NUM       0x01

/* String IDs */
#define UAC2_STR_MANUFACTURER         1
#define UAC2_STR_PRODUCT              2
#define UAC2_STR_SERIAL               3
#define UAC2_STR_CONFIG               4
#define UAC2_STR_CLOCK_SOURCE         5
#define UAC2_STR_LANGUAGE             0x0409 /* en-us */
#define UAC2_NSTRIDS                  5

#define UAC2_EP0MAXPACKET             64

extern const struct usb_devdesc_s g_uac2_device_desc;
extern const uint8_t g_uac2_config_desc_hs[];
extern const uint16_t g_uac2_config_desc_hs_len;

/* Descriptor helpers (pattern follows cdcacm_mk*desc) */
FAR const struct usb_devdesc_s *uac2_getdevdesc(void);
#ifdef CONFIG_USBDEV_DUALSPEED
FAR const struct usb_qualdesc_s *uac2_getqualdesc(void);
int16_t uac2_mkcfgdesc(FAR uint8_t *buf, uint8_t speed, uint8_t type);
#else
int16_t uac2_mkcfgdesc(FAR uint8_t *buf);
#endif
int uac2_mkstrdesc(uint8_t id, FAR struct usb_strdesc_s *strdesc);

#endif /* __UAC2_DESC_H */
