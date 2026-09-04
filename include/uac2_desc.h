/**
 * @file uac2_desc.h
 * @brief USB Audio Class 2.0 Descriptor Definitions for Spresense
 */

#ifndef __UAC2_DESC_H
#define __UAC2_DESC_H

#include <stdint.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev_desc.h>
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

/* Endpoint Addresses */
#define UAC2_EP_ISO_OUT               0x01 /* OUT Endpoint 1 */
#define UAC2_EP_FEEDBACK_IN           0x81 /* IN Endpoint 1 (Feedback) */

/* String IDs */
#define UAC2_STR_MANUFACTURER         1
#define UAC2_STR_PRODUCT              2
#define UAC2_STR_SERIAL               3
#define UAC2_STR_CONFIG               4
#define UAC2_STR_CLOCK_SOURCE         5

extern const struct usb_devdesc_s g_uac2_device_desc;
extern const uint8_t g_uac2_config_desc_hs[];
extern const uint16_t g_uac2_config_desc_hs_len;

#endif /* __UAC2_DESC_H */
