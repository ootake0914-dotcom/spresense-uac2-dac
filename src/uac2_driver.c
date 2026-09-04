/**
 * @file uac2_driver.c
 * @brief NuttX USB Device Class Driver for UAC2 192kHz/24bit DAC
 *
 * Phase 1: Enumeration. Implements the full standard-request set
 * (GET_DESCRIPTOR / SET+GET_CONFIGURATION / SET+GET_INTERFACE) following
 * the cdcacm/usbmsc reference pattern, plus minimal UAC2 class requests
 * (Clock freq CUR+RANGE, clock valid, Feature mute/volume) so Windows /
 * macOS / Linux UAC2 drivers complete enumeration without yellow-bang.
 *
 * NOTE: CXD5602 DCD (cxd56_usbdev.c) currently exposes only BULK/INT EPs.
 * ISO allocation will return NULL until the Phase-2 DCD ISOC patch lands.
 * Enumeration (Phase 1) does NOT require ISO EPs: SET_CONFIGURATION and
 * Alt-0 SET_INTERFACE succeed without touching ISO EPs. Alt-1 succeeds
 * opportunistically (configures EPs when available, else still ACKs).
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
typedef struct
{
  struct usbdevclass_driver_s drvr;
  struct usbdev_s *dev;
  struct usbdev_ep_s *ep_out;       /* ISO OUT (Audio Stream, EP2 OUT) */
  struct usbdev_ep_s *ep_fb;        /* ISO IN (Feedback, EP1 IN) */
  struct usbdev_req_s *ctrlreq;     /* EP0 Control Request */
  struct usbdev_req_s *outreq;      /* EP OUT Isochronous Request */
  struct usbdev_req_s *fbreq;       /* Feedback IN request */

  uint8_t config;                   /* Current configuration value */
  uint8_t alt_setting;              /* AS interface alt: 0 idle, 1 streaming */

  Uac2RingBuffer ringbuf;
  uint32_t sample_rate;             /* Current sample rate (192000) */
  bool mute[3];                     /* Master, Ch1, Ch2 */
  int16_t volume[3];                /* 8.8 fixed-point dB, 0 = 0dB */
  bool is_streaming;
} Uac2Driver;

static Uac2Driver g_uac2_dev;

/* Forward Declarations */
static int  uac2_bind(struct usbdevclass_driver_s *drvr,
                      struct usbdev_s *dev);
static void uac2_unbind(struct usbdevclass_driver_s *drvr,
                        struct usbdev_s *dev);
static int  uac2_setup(struct usbdevclass_driver_s *drvr,
                       struct usbdev_s *dev,
                       const struct usb_ctrlreq_s *ctrl,
                       uint8_t *dataout, size_t outlen);
static void uac2_disconnect(struct usbdevclass_driver_s *drvr,
                            struct usbdev_s *dev);

/* USB Device Class Driver Operations Table */
static const struct usbdevclass_driverops_s g_uac2_driverops =
{
  .bind       = uac2_bind,
  .unbind     = uac2_unbind,
  .setup      = uac2_setup,
  .disconnect = uac2_disconnect,
  .suspend    = NULL,
  .resume     = NULL,
};

static void uac2_ep0_complete(struct usbdev_ep_s *ep,
                              struct usbdev_req_s *req)
{
  /* Control transfer completed. Flag errors for usb trace. */
  if (req->result != 0 || req->xfrd != req->len)
    {
      uinfo("EP0 complete result=%d xfrd=%u len=%u\n",
            req->result, req->xfrd, req->len);
    }
}

static void uac2_iso_out_complete(struct usbdev_ep_s *ep,
                                  struct usbdev_req_s *req)
{
  /* Audio stream packet received (125us microframe interval) */
  if (req->result == 0 && req->xfrd > 0)
    {
      uac2_ringbuf_write(&g_uac2_dev.ringbuf, req->buf, req->xfrd);
    }

  /* Re-queue the request for next packet */
  if (g_uac2_dev.is_streaming && g_uac2_dev.ep_out && req == g_uac2_dev.outreq)
    {
      req->len = UAC2_PACKET_SIZE_192K;
      EP_SUBMIT(ep, req);
    }
}

static void uac2_fb_in_complete(struct usbdev_ep_s *ep,
                                struct usbdev_req_s *req)
{
  /* Phase 4: feedback IN complete. Currently one-shot; re-submit while
   * streaming so host keeps receiving clock ratio.
   */
  if (g_uac2_dev.is_streaming && g_uac2_dev.ep_fb && req == g_uac2_dev.fbreq &&
      req->result == 0)
    {
      /* TODO Phase 4: compute real Ff from SOF vs CXD5247 MCLK.
       * For now hold nominal 192kHz ratio: Ff = Fs * 2^13 for HS?
       * UAC2 feedback is 16.16 or 12.13; send nominal 192000.
       */
      EP_SUBMIT(ep, req);
    }
}

/* Helpers: endpoint configure descriptors built from static table values */

static void uac2_build_iso_out_desc(FAR struct usb_epdesc_s *epdesc)
{
  epdesc->len  = USB_SIZEOF_EPDESC;
  epdesc->type = USB_DESC_TYPE_ENDPOINT;
  epdesc->addr = UAC2_EP_ISO_OUT;
  epdesc->attr = 0x05; /* Isochronous, Asynchronous */
  epdesc->mxpacketsize[0] = (uint8_t)(UAC2_PACKET_SIZE_192K & 0xFF);
  epdesc->mxpacketsize[1] = (uint8_t)(UAC2_PACKET_SIZE_192K >> 8);
  epdesc->interval = 0x01;
}

static void uac2_build_fb_in_desc(FAR struct usb_epdesc_s *epdesc)
{
  epdesc->len  = USB_SIZEOF_EPDESC;
  epdesc->type = USB_DESC_TYPE_ENDPOINT;
  epdesc->addr = UAC2_EP_FEEDBACK_IN;
  epdesc->attr = 0x11; /* Isochronous, Feedback */
  epdesc->mxpacketsize[0] = 0x04;
  epdesc->mxpacketsize[1] = 0x00;
  epdesc->interval = 0x04; /* 8 uframes = 1ms */
}

static void uac2_resetconfig(Uac2Driver *priv)
{
  if (priv->config != 0)
    {
      priv->config = 0;
    }

  priv->is_streaming = false;
  priv->alt_setting  = 0;

  /* Disable streaming EPs if they were configured. Outstanding
   * transfers complete with -ESHUTDOWN.
   */

  if (priv->ep_out)
    {
      EP_DISABLE(priv->ep_out);
    }

  if (priv->ep_fb)
    {
      EP_DISABLE(priv->ep_fb);
    }
}

static int uac2_setconfig(Uac2Driver *priv, uint8_t config)
{
  if (config == priv->config)
    {
      return 0;
    }

  uac2_resetconfig(priv);

  if (config == 0)
    {
      return 0;
    }

  if (config != UAC2_CONFIG_ID)
    {
      return -EINVAL;
    }

  /* UAC2 config has no EPs active in Alt-0 (zero-bandwidth).
   * Nothing to configure here; EPs are configured on SET_INTERFACE Alt-1.
   */

  priv->config = config;
  return 0;
}

static int uac2_setinterface(Uac2Driver *priv, uint8_t ifno, uint8_t alt)
{
  if (priv->config != UAC2_CONFIG_ID)
    {
      return -EINVAL;
    }

  if (ifno == UAC2_IF_AUDIO_CONTROL)
    {
      /* AC interface has only Alt-0 */
      return (alt == 0) ? 0 : -EINVAL;
    }

  if (ifno != UAC2_IF_AUDIO_STREAMING)
    {
      return -EINVAL;
    }

  if (alt > 1)
    {
      return -EINVAL;
    }

  if (alt == priv->alt_setting && ((alt == 1) == priv->is_streaming))
    {
      return 0;
    }

  if (alt == 0)
    {
      /* Stop streaming */
      priv->is_streaming = false;
      priv->alt_setting  = 0;
      if (priv->ep_out)
        {
          EP_DISABLE(priv->ep_out);
        }

      if (priv->ep_fb)
        {
          EP_DISABLE(priv->ep_fb);
        }

      return 0;
    }

  /* Alt-1: start streaming. Configure EPs when the DCD provides them.
   * If ISOC EPs are unavailable (pre-Phase-2 DCD), still ACK so that
   * enumeration / alt-probe completes; streaming actually starts once
   * the DCD ISOC patch lands.
   */

  priv->alt_setting  = 1;
  priv->is_streaming = true;

  if (priv->ep_out)
    {
      struct usb_epdesc_s epdesc;
      uac2_build_iso_out_desc(&epdesc);
      EP_CONFIGURE(priv->ep_out, &epdesc, false);

      if (!priv->outreq)
        {
          priv->outreq = usbdev_allocreq(priv->ep_out, UAC2_PACKET_SIZE_192K);
          if (priv->outreq)
            {
              priv->outreq->callback = uac2_iso_out_complete;
            }
        }

      if (priv->outreq)
        {
          priv->outreq->len = UAC2_PACKET_SIZE_192K;
          EP_SUBMIT(priv->ep_out, priv->outreq);
        }
    }

  if (priv->ep_fb)
    {
      struct usb_epdesc_s epdesc;
      uac2_build_fb_in_desc(&epdesc);
      EP_CONFIGURE(priv->ep_fb, &epdesc, true);

      if (!priv->fbreq)
        {
          priv->fbreq = usbdev_allocreq(priv->ep_fb, 4);
          if (priv->fbreq)
            {
              priv->fbreq->callback = uac2_fb_in_complete;
              /* Nominal feedback payload (Phase 4 will update): 192kHz */
              priv->fbreq->buf[0] = 0x00;
              priv->fbreq->buf[1] = 0x00;
              priv->fbreq->buf[2] = 0x06;
              priv->fbreq->buf[3] = 0x00;
              priv->fbreq->len    = 4;
            }
        }

      if (priv->fbreq)
        {
          priv->fbreq->len = 4;
          EP_SUBMIT(priv->ep_fb, priv->fbreq);
        }
    }

  return 0;
}

static int uac2_bind(struct usbdevclass_driver_s *drvr,
                     struct usbdev_s *dev)
{
  Uac2Driver *priv = (Uac2Driver *)drvr;

  priv->dev = dev;
  dev->ep0->priv = priv;

  /* Allocate EP0 control request */
  priv->ctrlreq = usbdev_allocreq(dev->ep0, UAC2_MXDESCLEN);
  if (!priv->ctrlreq)
    {
      return -ENOMEM;
    }

  priv->ctrlreq->callback = uac2_ep0_complete;

  /* Pre-allocate ISO endpoints. May return NULL until the CXD56 DCD
   * gains ISOC support (Phase 2). Enumeration does not depend on them.
   */

  priv->ep_out = DEV_ALLOCEP(dev, UAC2_EP_ISO_OUT_NUM, false,
                             USB_EP_ATTR_XFER_ISOC);
  if (priv->ep_out)
    {
      priv->ep_out->priv = priv;
    }
  else
    {
      uinfo("ISO OUT EP unavailable (expected pre-ISOC DCD patch)\n");
    }

  priv->ep_fb = DEV_ALLOCEP(dev, UAC2_EP_FEEDBACK_IN_NUM, true,
                            USB_EP_ATTR_XFER_ISOC);
  if (priv->ep_fb)
    {
      priv->ep_fb->priv = priv;
    }
  else
    {
      uinfo("Feedback IN EP unavailable (expected pre-ISOC DCD patch)\n");
    }

  /* Initialize audio state */
  uac2_ringbuf_init(&priv->ringbuf);
  priv->config       = 0;
  priv->sample_rate  = UAC2_SAMPLE_RATE_192K;
  priv->alt_setting  = 0;
  priv->is_streaming = false;
  memset(priv->mute, 0, sizeof(priv->mute));
  memset(priv->volume, 0, sizeof(priv->volume));
  priv->outreq = NULL;
  priv->fbreq  = NULL;

#ifdef CONFIG_USBDEV_SELFPOWERED
  DEV_SETSELFPOWERED(dev);
#endif

  DEV_CONNECT(dev);
  return 0;
}

static void uac2_unbind(struct usbdevclass_driver_s *drvr,
                        struct usbdev_s *dev)
{
  Uac2Driver *priv = (Uac2Driver *)drvr;

  uac2_resetconfig(priv);

  if (priv->outreq && priv->ep_out)
    {
      usbdev_freereq(priv->ep_out, priv->outreq);
      priv->outreq = NULL;
    }

  if (priv->fbreq && priv->ep_fb)
    {
      usbdev_freereq(priv->ep_fb, priv->fbreq);
      priv->fbreq = NULL;
    }

  if (priv->ctrlreq)
    {
      usbdev_freereq(dev->ep0, priv->ctrlreq);
      priv->ctrlreq = NULL;
    }

  if (priv->ep_out)
    {
      DEV_FREEEP(dev, priv->ep_out);
      priv->ep_out = NULL;
    }

  if (priv->ep_fb)
    {
      DEV_FREEEP(dev, priv->ep_fb);
      priv->ep_fb = NULL;
    }

  priv->dev = NULL;
}

/* UAC2 class-specific helpers */

static int uac2_clock_source_request(Uac2Driver *priv,
                                     const struct usb_ctrlreq_s *ctrl,
                                     uint8_t *dataout, size_t outlen,
                                     uint8_t cs, uint8_t req, uint16_t len)
{
  struct usbdev_req_s *ctrlreq = priv->ctrlreq;

  if (cs == UAC2_CS_CONTROL_SAM_FREQ)
    {
      if (req == UAC2_CS_CUR)
        {
          if (ctrl->type & USB_REQ_DIR_IN)
            {
              uint32_t freq = priv->sample_rate;
              memcpy(ctrlreq->buf, &freq, 4);
              return 4;
            }
          else
            {
              /* SET_CUR frequency: accept 192k (or store if multi-rate later) */
              if (dataout && outlen >= 4)
                {
                  uint32_t newfreq;
                  memcpy(&newfreq, dataout, 4);
                  if (newfreq == UAC2_SAMPLE_RATE_192K)
                    {
                      priv->sample_rate = newfreq;
                    }
                  else
                    {
                      /* Phase 1 is 192k-only; ACK anyway to keep host happy */
                      uinfo("SET_CUR freq %lu ignored (192k-only)\n",
                            (unsigned long)newfreq);
                    }
                }

              return 0;
            }
        }
      else if (req == UAC2_CS_RANGE)
        {
          /* One subrange: 192000..192000, res 0 */
          static const uint8_t range_desc[] =
          {
            0x01, 0x00,               /* wNumSubRanges: 1 */
            0x00, 0xEE, 0x02, 0x00,   /* dMIN: 192000 */
            0x00, 0xEE, 0x02, 0x00,   /* dMAX: 192000 */
            0x00, 0x00, 0x00, 0x00    /* dRES: 0 */
          };

          memcpy(ctrlreq->buf, range_desc, sizeof(range_desc));
          return sizeof(range_desc);
        }
    }
  else if (cs == UAC2_CS_CONTROL_CLOCK_VALID)
    {
      if (req == UAC2_CS_CUR && (ctrl->type & USB_REQ_DIR_IN))
        {
          ctrlreq->buf[0] = 0x01; /* Valid */
          return 1;
        }
      else if (req == UAC2_CS_CUR)
        {
          return 0;
        }
    }

  return -EOPNOTSUPP;
}

static int uac2_feature_unit_request(Uac2Driver *priv,
                                     const struct usb_ctrlreq_s *ctrl,
                                     uint8_t *dataout, size_t outlen,
                                     uint8_t cs, uint8_t ch, uint8_t req)
{
  struct usbdev_req_s *ctrlreq = priv->ctrlreq;
  uint8_t idx = (ch > 2) ? 0 : ch; /* clamp bogus channel to master */

  if (cs == 0x01) /* MUTE */
    {
      if (req == UAC2_CS_CUR)
        {
          if (ctrl->type & USB_REQ_DIR_IN)
            {
              ctrlreq->buf[0] = priv->mute[idx] ? 0x01 : 0x00;
              return 1;
            }
          else
            {
              if (dataout && outlen >= 1)
                {
                  priv->mute[idx] = (dataout[0] != 0);
                }

              return 0;
            }
        }
    }
  else if (cs == 0x02) /* VOLUME */
    {
      if (req == UAC2_CS_CUR)
        {
          if (ctrl->type & USB_REQ_DIR_IN)
            {
              memcpy(ctrlreq->buf, &priv->volume[idx], 2);
              return 2;
            }
          else
            {
              if (dataout && outlen >= 2)
                {
                  memcpy(&priv->volume[idx], dataout, 2);
                }

              return 0;
            }
        }
      else if (req == UAC2_CS_RANGE)
        {
          /* One subrange: MIN -60dB (0xC400), MAX 0dB, RES 1dB (0x0100) */
          static const uint8_t vol_range[] =
          {
            0x01, 0x00,   /* wNumSubRanges: 1 */
            0x00, 0xC4,   /* MIN Lo (-60dB LE: 0xC400) */
            0x00, 0x00,   /* MAX Lo (0dB) */
            0x00, 0x01    /* RES Lo (1dB) */
          };
          /* NOTE: minimal 8-byte form (wNumSubRanges + MIN/MAX/RES words)
           * accepted by Windows/macOS for volume. Full UAC2 RANGE uses
           * 4-byte d-words, but 2-byte form is what hosts probe here.
           */
          memcpy(ctrlreq->buf, vol_range, sizeof(vol_range));
          return sizeof(vol_range);
        }
      else if (req == 0x03) /* MIN */
        {
          ctrlreq->buf[0] = 0x00;
          ctrlreq->buf[1] = 0xC4;
          return 2;
        }
      else if (req == 0x04) /* MAX */
        {
          ctrlreq->buf[0] = 0x00;
          ctrlreq->buf[1] = 0x00;
          return 2;
        }
      else if (req == 0x05) /* RES */
        {
          ctrlreq->buf[0] = 0x00;
          ctrlreq->buf[1] = 0x01;
          return 2;
        }
    }

  return -EOPNOTSUPP;
}

static int uac2_setup(struct usbdevclass_driver_s *drvr,
                      struct usbdev_s *dev,
                      const struct usb_ctrlreq_s *ctrl,
                      uint8_t *dataout, size_t outlen)
{
  Uac2Driver *priv = (Uac2Driver *)drvr;
  struct usbdev_req_s *ctrlreq;
  uint8_t type = ctrl->type;
  uint8_t req = ctrl->req;
  uint16_t value = GETUINT16(ctrl->value);
  uint16_t index = GETUINT16(ctrl->index);
  uint16_t len = GETUINT16(ctrl->len);
  int ret = -EOPNOTSUPP;

  if (!priv || !priv->ctrlreq)
    {
      return -ENODEV;
    }

  ctrlreq = priv->ctrlreq;

  uinfo("type=%02x req=%02x value=%04x index=%04x len=%04x\n",
        type, req, value, index, len);

  /* Standard Requests */
  if ((type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD)
    {
      switch (req)
        {
        case USB_REQ_GETDESCRIPTOR:
          {
            uint8_t desc_type = ctrl->value[1];
            uint8_t desc_idx  = ctrl->value[0];

            switch (desc_type)
              {
              case USB_DESC_TYPE_DEVICE:
                ret = USB_SIZEOF_DEVDESC;
                memcpy(ctrlreq->buf, uac2_getdevdesc(), ret);
                break;

#ifdef CONFIG_USBDEV_DUALSPEED
              case USB_DESC_TYPE_DEVICEQUALIFIER:
                ret = USB_SIZEOF_QUALDESC;
                memcpy(ctrlreq->buf, uac2_getqualdesc(), ret);
                break;

              case USB_DESC_TYPE_OTHERSPEEDCONFIG:
                ret = uac2_mkcfgdesc(ctrlreq->buf, dev->speed,
                                     USB_DESC_TYPE_OTHERSPEEDCONFIG);
                break;
#endif

              case USB_DESC_TYPE_CONFIG:
#ifdef CONFIG_USBDEV_DUALSPEED
                ret = uac2_mkcfgdesc(ctrlreq->buf, dev->speed,
                                     USB_DESC_TYPE_CONFIG);
#else
                ret = uac2_mkcfgdesc(ctrlreq->buf);
#endif
                break;

              case USB_DESC_TYPE_STRING:
                ret = uac2_mkstrdesc(desc_idx,
                                     (FAR struct usb_strdesc_s *)ctrlreq->buf);
                break;

              default:
                break;
              }
          }
          break;

        case USB_REQ_SETCONFIGURATION:
          if (type == 0)
            {
              ret = uac2_setconfig(priv, (uint8_t)value);
              if (ret == 0)
                {
                  /* Zero-length status stage; fall through to submit below
                   * with ret == 0.
                   */
                }
            }
          break;

        case USB_REQ_GETCONFIGURATION:
          if (type == USB_DIR_IN)
            {
              ctrlreq->buf[0] = priv->config;
              ret = 1;
            }
          break;

        case USB_REQ_SETINTERFACE:
          if ((type & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_INTERFACE)
            {
              ret = uac2_setinterface(priv, (uint8_t)index, (uint8_t)value);
            }
          break;

        case USB_REQ_GETINTERFACE:
          if (type == (USB_DIR_IN | USB_REQ_RECIPIENT_INTERFACE))
            {
              if (index == UAC2_IF_AUDIO_CONTROL)
                {
                  ctrlreq->buf[0] = 0;
                  ret = 1;
                }
              else if (index == UAC2_IF_AUDIO_STREAMING)
                {
                  ctrlreq->buf[0] = priv->alt_setting;
                  ret = 1;
                }
            }
          break;

        default:
          break;
        }
    }
  /* UAC2 Class-Specific Requests (Interface recipient) */
  else if ((type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS)
    {
      uint8_t entity_id  = (uint8_t)(index >> 8);
      uint8_t cs         = (uint8_t)(value >> 8);
      uint8_t ch         = (uint8_t)(value & 0xFF);

      if (entity_id == UAC2_ENTITY_CLOCK_SOURCE)
        {
          ret = uac2_clock_source_request(priv, ctrl, dataout, outlen,
                                          cs, req, len);
        }
      else if (entity_id == UAC2_ENTITY_FEATURE_UNIT)
        {
          ret = uac2_feature_unit_request(priv, ctrl, dataout, outlen,
                                          cs, ch, req);
        }
      else if (entity_id == UAC2_ENTITY_INPUT_TERMINAL ||
               entity_id == UAC2_ENTITY_OUTPUT_TERMINAL)
        {
          /* Terminal COPY/CONNECTOR/OVERLOAD etc: no controls -> stall
           * is correct, but some hosts probe; return 0-length ACK for
           * CUR to avoid yellow-bang, stall otherwise.
           */
          if (req == UAC2_CS_CUR && (type & USB_REQ_DIR_IN))
            {
              ctrlreq->buf[0] = 0x00;
              ret = 1;
            }
          else if (req == UAC2_CS_CUR)
            {
              ret = 0;
            }
        }
    }

  /* Respond to the setup command if data was returned. On error
   * (ret < 0) the DCD will stall.
   */

  if (ret >= 0)
    {
      ctrlreq->len   = (len < (uint16_t)ret) ? len : (uint16_t)ret;
      ctrlreq->flags = USBDEV_REQFLAGS_NULLPKT;

      ret = EP_SUBMIT(dev->ep0, ctrlreq);
      if (ret < 0)
        {
          ctrlreq->result = OK;
          uac2_ep0_complete(dev->ep0, ctrlreq);
        }
    }

  return ret;
}

static void uac2_disconnect(struct usbdevclass_driver_s *drvr,
                            struct usbdev_s *dev)
{
  Uac2Driver *priv = (Uac2Driver *)drvr;

  uac2_resetconfig(priv);

  /* Re-arm pull-up so the device re-enumerates on next attach */
  DEV_CONNECT(dev);
}

/**
 * @brief Initialize and register UAC2 Device Class
 */
int uac2_driver_register(void)
{
  memset(&g_uac2_dev, 0, sizeof(Uac2Driver));
#ifdef CONFIG_USBDEV_DUALSPEED
  g_uac2_dev.drvr.speed = USB_SPEED_HIGH;
#else
  g_uac2_dev.drvr.speed = USB_SPEED_FULL;
#endif
  g_uac2_dev.drvr.ops   = &g_uac2_driverops;

  return usbdev_register(&g_uac2_dev.drvr);
}


void uac2_get_status(bool *is_streaming, uint8_t *alt_setting, uint32_t *sample_rate,
                     uint32_t *underrun, uint32_t *overrun, uint32_t *buffered)
{
  if (is_streaming) *is_streaming = g_uac2_dev.is_streaming;
  if (alt_setting) *alt_setting = g_uac2_dev.alt_setting;
  if (sample_rate) *sample_rate = g_uac2_dev.sample_rate;
  if (underrun) *underrun = g_uac2_dev.ringbuf.underrun_count;
  if (overrun) *overrun = g_uac2_dev.ringbuf.overrun_count;
  if (buffered) *buffered = uac2_ringbuf_available_read(&g_uac2_dev.ringbuf);
}
