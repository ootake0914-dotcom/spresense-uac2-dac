/**
 * @file uac2_driver.c
 * @brief NuttX USB Device Class Driver for UAC2 192kHz/24bit DAC
 */

#include <nuttx/config.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/audio.h>

#include "uac2.h"
#include "uac2_desc.h"
#include "uac2_ringbuf.h"

/* UAC2 Class Driver State Structure */
typedef struct {
    struct usbdevclass_driver_s drvr;
    struct usbdev_s *dev;
    struct usbdev_ep_s *ep0;
    struct usbdev_ep_s *ep_out;       /* ISO OUT (Audio Stream) */
    struct usbdev_ep_s *ep_fb;        /* ISO IN (Clock Feedback) */
    struct usbdev_req_s *ctrlreq;     /* EP0 Control Request */
    struct usbdev_req_s *outreq;      /* EP OUT Isochronous Request */

    Uac2RingBuffer ringbuf;
    uint32_t sample_rate;             /* Current sample rate (192000) */
    uint8_t alt_setting;              /* 0: Idle, 1: Streaming */
    bool mute;
    int16_t volume_db;                /* 8.8 fixed-point dB */
    bool is_streaming;
} Uac2Driver;

static Uac2Driver g_uac2_dev;

/* Forward Declarations */
static int  uac2_bind(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev);
static void uac2_unbind(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev);
static int  uac2_setup(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev,
                       const struct usb_ctrlreq_s *ctrl, uint8_t *dataout, size_t outlen);
static void uac2_disconnect(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev);

/* USB Device Class Driver Operations Table */
static const struct usbdevclass_driverops_s g_uac2_driverops = {
    .bind       = uac2_bind,
    .unbind     = uac2_unbind,
    .setup      = uac2_setup,
    .disconnect = uac2_disconnect,
    .suspend    = NULL,
    .resume     = NULL,
};

static void uac2_ep0_complete(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
    /* Control transfer completed */
    (void)ep;
    (void)req;
}

static void uac2_iso_out_complete(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
    /* Audio stream packet received (125us microframe interval) */
    if (req->result == 0 && req->xfrd > 0) {
        uac2_ringbuf_write(&g_uac2_dev.ringbuf, req->buf, req->xfrd);
    }

    /* Re-queue the request for next packet */
    if (g_uac2_dev.is_streaming && g_uac2_dev.ep_out) {
        req->len = UAC2_PACKET_SIZE_192K;
        EP_SUBMIT(ep, req);
    }
}

static int uac2_bind(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev)
{
    Uac2Driver *priv = (Uac2Driver *)drvr;
    priv->dev = dev;
    priv->ep0 = dev->ep0;

    /* Allocate EP0 request */
    priv->ctrlreq = EP_ALLOCREQ(priv->ep0, 256);
    if (!priv->ctrlreq) {
        return -ENOMEM;
    }
    priv->ctrlreq->callback = uac2_ep0_complete;

    /* Initialize Ring Buffer & Audio State */
    uac2_ringbuf_init(&priv->ringbuf);
    priv->sample_rate = UAC2_SAMPLE_RATE_192K;
    priv->alt_setting = 0;
    priv->mute = false;
    priv->volume_db = 0;
    priv->is_streaming = false;

    return 0;
}

static void uac2_unbind(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev)
{
    Uac2Driver *priv = (Uac2Driver *)drvr;
    if (priv->ctrlreq) {
        EP_FREEREQ(priv->ep0, priv->ctrlreq);
        priv->ctrlreq = NULL;
    }
    priv->dev = NULL;
}

static int uac2_setup(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev,
                       const struct usb_ctrlreq_s *ctrl, uint8_t *dataout, size_t outlen)
{
    Uac2Driver *priv = (Uac2Driver *)drvr;
    uint8_t type = ctrl->type;
    uint8_t req = ctrl->req;
    uint16_t value = GETUINT16(ctrl->value);
    uint16_t index = GETUINT16(ctrl->index);
    uint16_t len = GETUINT16(ctrl->len);
    int ret = -EOPNOTSUPP;

    /* Standard Requests */
    if ((type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        switch (req) {
        case USB_REQ_SETINTERFACE:
            if (index == UAC2_IF_AUDIO_STREAMING) {
                priv->alt_setting = (uint8_t)value;
                if (priv->alt_setting == 1) {
                    /* Start Streaming */
                    priv->is_streaming = true;
                    if (!priv->ep_out) {
                        priv->ep_out = DEV_ALLOCCONTROLLER(dev, UAC2_EP_ISO_OUT, false, USB_EP_ATTR_XFER_ISOC);
                        if (priv->ep_out) {
                            priv->outreq = EP_ALLOCREQ(priv->ep_out, UAC2_PACKET_SIZE_192K);
                            if (priv->outreq) {
                                priv->outreq->callback = uac2_iso_out_complete;
                                priv->outreq->len = UAC2_PACKET_SIZE_192K;
                                EP_SUBMIT(priv->ep_out, priv->outreq);
                            }
                        }
                    }
                } else {
                    /* Stop Streaming */
                    priv->is_streaming = false;
                }
                return 0;
            }
            break;
        case USB_REQ_GETINTERFACE:
            if (index == UAC2_IF_AUDIO_STREAMING) {
                priv->ctrlreq->buf[0] = priv->alt_setting;
                priv->ctrlreq->len = 1;
                return EP_SUBMIT(priv->ep0, priv->ctrlreq);
            }
            break;
        default:
            break;
        }
    }

    /* UAC2 Class-Specific Requests */
    if ((type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
        uint8_t entity_id = (uint8_t)(index >> 8);
        uint8_t control_sel = (uint8_t)(value >> 8);

        /* Clock Source Entity (192kHz Sampling Rate) */
        if (entity_id == UAC2_ENTITY_CLOCK_SOURCE) {
            if (control_sel == UAC2_CS_CONTROL_SAM_FREQ) {
                if (req == UAC2_CS_CUR) {
                    if (type & USB_REQ_DIR_IN) {
                        /* Return current frequency: 192000 Hz */
                        uint32_t freq = priv->sample_rate;
                        memcpy(priv->ctrlreq->buf, &freq, 4);
                        priv->ctrlreq->len = 4;
                        return EP_SUBMIT(priv->ep0, priv->ctrlreq);
                    } else {
                        /* Host requests to set frequency */
                        return 0;
                    }
                } else if (req == UAC2_CS_RANGE) {
                    /* Supported Range: 192000 - 192000, step 0 */
                    uint8_t range_desc[] = {
                        0x01, 0x00,                         /* wNumSubRanges: 1 */
                        0x00, 0xEE, 0x02, 0x00,             /* dMIN: 192000 */
                        0x00, 0xEE, 0x02, 0x00,             /* dMAX: 192000 */
                        0x00, 0x00, 0x00, 0x00              /* dRES: 0 */
                    };
                    memcpy(priv->ctrlreq->buf, range_desc, sizeof(range_desc));
                    priv->ctrlreq->len = sizeof(range_desc);
                    return EP_SUBMIT(priv->ep0, priv->ctrlreq);
                }
            }
        }
    }

    return ret;
}

static void uac2_disconnect(struct usbdevclass_driver_s *drvr, struct usbdev_s *dev)
{
    Uac2Driver *priv = (Uac2Driver *)drvr;
    priv->is_streaming = false;
    priv->alt_setting = 0;
}

/**
 * @brief Initialize and register UAC2 Device Class
 */
int uac2_driver_register(void)
{
    memset(&g_uac2_dev, 0, sizeof(Uac2Driver));
    g_uac2_dev.drvr.speed = USB_SPEED_HIGH;
    g_uac2_dev.drvr.ops   = &g_uac2_driverops;

    return usbdev_register(&g_uac2_dev.drvr);
}
