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
#include <stdio.h>
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
#include "uac2_audio_dma.h"


/* Rev86c-diag (temporary): EP1-IN interrupt accounting from the DCD
 * (cxd56_usbdev.c g_ep1_irq_cnt). Tells us which status bits fire for
 * isochronous IN: IN-token / XFERDONE / ISO_IN_DONE / BNA / HE / ...
 */
extern volatile uint32_t g_ep1_irq_cnt[8];

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
  volatile bool fb_inflight;        /* Rev76: paced submit guard (ISR clears) */

  /* Rev85: feedback double-buffer + submit accounting.
   * The DMA-owned fbreq->buf must NEVER be touched while fb_inflight.
   * uac2_feedback_update() (pump thread) only stages into fb_pending_q16;
   * uac2_feedback_poll() copies pending -> HW buf at submit time (idle).
   * Counters are volatile telemetry (no UART in steady state).
   */
  volatile uint32_t fb_pending_q16;   /* staged PI output (pump -> poll handoff) */
  volatile bool fb_has_pending;     /* pending value waiting for submit */
  volatile uint32_t fb_last_sent_q16; /* last value copied to HW buf */
  volatile uint32_t fb_submit_ok;   /* EP_SUBMIT success count */
  volatile uint32_t fb_submit_fail; /* EP_SUBMIT sync-failure count */
  volatile uint32_t fb_complete_cnt;/* IN-complete callback count */

  uint8_t config;                   /* Current configuration value */
  uint8_t config_applied;           /* HW applied config (task context) */
  uint8_t alt_setting;              /* AS interface alt: requested (ISR context) */
  uint8_t alt_applied;              /* AS interface alt: hardware applied (task context) */

  /* 死にringbuf撤去済み（実給電はuac2_audio_dma.c側g_pcm_ringのみ） */
  uint32_t sample_rate;             /* Current sample rate (192000) */
  bool mute[3];                     /* Master, Ch1, Ch2 */
  int16_t volume[3];                /* 8.8 fixed-point dB, 0 = 0dB */
  bool is_streaming;
} Uac2Driver;

static Uac2Driver g_uac2_dev;

/* Lightweight lock-free EP0 SETUP trace queue for zero-overhead debug */
typedef struct
{
  uint8_t  type;
  uint8_t  req;
  uint16_t value;
  uint16_t index;
  uint16_t len;
  int16_t  ret;
} Uac2SetupLog;

#define UAC2_LOG_SIZE 128
static Uac2SetupLog g_setup_logs[UAC2_LOG_SIZE];
static volatile uint16_t g_log_head = 0;
static volatile uint16_t g_log_tail = 0;

static inline void uac2_log_setup(uint8_t type, uint8_t req, uint16_t value,
                                  uint16_t index, uint16_t len, int16_t ret)
{
  uint16_t next = (g_log_head + 1) % UAC2_LOG_SIZE;
  if (next != g_log_tail)
    {
      g_setup_logs[g_log_head].type  = type;
      g_setup_logs[g_log_head].req   = req;
      g_setup_logs[g_log_head].value = value;
      g_setup_logs[g_log_head].index = index;
      g_setup_logs[g_log_head].len   = len;
      g_setup_logs[g_log_head].ret   = ret;
      g_log_head = next;
    }
}

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

/* Max ISO OUT packet across Alts (Alt2 24-bit): request buffers use this. */
#define UAC2_ISO_OUT_MAXPKT UAC2_PACKET_SIZE_24BIT_192K

static uint16_t uac2_alt_packet_size(uint8_t alt)
{
#if UAC2_SINGLE_ALT0_STREAMING
  return UAC2_PACKET_SIZE_24BIT_192K;
#else
  /* Alt1 = 16-bit PCM (100B max), Alt2 = 24-bit PCM in 32-bit slot (200B). */
  return (alt == 1) ? UAC2_PACKET_SIZE_16BIT_192K : UAC2_PACKET_SIZE_24BIT_192K;
#endif
}

static void uac2_iso_out_complete(struct usbdev_ep_s *ep,
                                   struct usbdev_req_s *req)
{
  static uint32_t s_pkt_count = 0;
  static uint32_t s_badlen_count = 0;

  /* Audio stream packet received (125us microframe interval) */
  if (req->result == 0 && req->xfrd > 0)
    {
      /* P0: 8-byte stereo-frame guard. A non-multiple-of-8 packet means
       * wire corruption; accepting it (or truncating it) would shift every
       * subsequent frame and turn all following audio into noise until
       * stream restart. Drop the whole packet: a 125us gap preserves
       * alignment (audible click at worst). Oversize is impossible with
       * the fixed MAXPKT buffer, but guarded anyway.
       */
      if (req->xfrd > UAC2_ISO_OUT_MAXPKT || (req->xfrd & 7u) != 0)
        {
          s_badlen_count++;
#if !UAC2_SILENT_DIAG
          if (s_badlen_count <= 5 || (s_badlen_count % 1000 == 0))
            {
              UAC2_TPRINTF("[ISO_OUT] BADLEN #%lu: %u bytes dropped (8B guard)\n",
                     (unsigned long)s_badlen_count, (unsigned)req->xfrd);
            }
#endif
        }
      else
        {
          s_pkt_count++;
#if !UAC2_SILENT_DIAG
          if (s_pkt_count <= 5 || (s_pkt_count % 8000 == 0))
            {
              UAC2_TPRINTF("[ISO_OUT] pkt #%lu: %u bytes received!\n",
                     (unsigned long)s_pkt_count, (unsigned)req->xfrd);
            }
#endif
          /* 給電はg_pcm_ring（uac2_audio_dma.c側）のみ。二重書き込み排除
           * （Claude指摘：g_uac2_dev.ringbufは誰にもreadされず統計が腐る）。
           */
          uac2_audio_write(req->buf, req->xfrd);
        }
    }

  /* Re-queue the request for next packet. Buffer is sized for the max
   * Alt (Alt2 24-bit: 200 B); smaller Alt1 (16-bit) packets fit inside.
   */
  if (g_uac2_dev.is_streaming && g_uac2_dev.ep_out && req == g_uac2_dev.outreq)
    {
      req->len = UAC2_ISO_OUT_MAXPKT;
      EP_SUBMIT(ep, req);
    }
}

static void uac2_fb_in_complete(struct usbdev_ep_s *ep,
                                 struct usbdev_req_s *req)
{
  /* Rev76 paced submit: the IN-complete callback (USB IRQ context) only
   * releases the slot and NEVER resubmits. Resubmission is paced at <=1kHz
   * from the pump thread (task context) via uac2_feedback_poll(), so even
   * a hyperactive controller cannot build an ISR resubmit storm.
   * Format (verified): HS Q16.16 LE, samples/microframe.
   * Nominal 192kHz = 24.0 = 0x00180000.
   * Rev85: count completions for stuck-IN diagnosis (Rev83: IN never
   * completes, DMA descriptor pristine, wire shows 4 zero bytes @1kHz).
   */
  if (g_uac2_dev.ep_fb && req == g_uac2_dev.fbreq)
    {
      g_uac2_dev.fb_inflight = false;
      g_uac2_dev.fb_complete_cnt++;
    }
}

/* Rev85: async-feedback payload stager (pump thread, task context).
 * Double-buffered: stages the PI output into fb_pending_q16 WITHOUT
 * touching the DMA-owned fbreq->buf. The copy happens in
 * uac2_feedback_poll() at submit time when no transfer is in flight.
 * Overwriting fbreq->buf while fb_inflight would corrupt an active
 * DMA transfer (future silicon where IN completions actually fire).
 */
void uac2_feedback_update(uint32_t ff_q16)
{
  g_uac2_dev.fb_pending_q16 = ff_q16;
  g_uac2_dev.fb_has_pending = true;
}

/* Rev85: feedback submit telemetry (debugger / future MON wiring).
 * Snapshot struct is frozen (ASMP ABI v3); expose via accessor so the
 * stuck-IN state (ok>0, done==0) is observable without layout churn.
 */
void uac2_feedback_stats(uint32_t *ok, uint32_t *fail, uint32_t *done,
                         bool *inflight, uint32_t *last_sent)
{
  if (ok)        *ok        = g_uac2_dev.fb_submit_ok;
  if (fail)      *fail      = g_uac2_dev.fb_submit_fail;
  if (done)      *done      = g_uac2_dev.fb_complete_cnt;
  if (inflight)  *inflight  = g_uac2_dev.fb_inflight;
  if (last_sent) *last_sent = g_uac2_dev.fb_last_sent_q16;
}

/* Rev85: paced feedback submitter (pump thread, task context, <=1kHz).
 * Submits the 4B Q16.16 payload only when streaming and no transfer is
 * in flight. The host polls EP1 every 1ms (bInterval=4); a missed poll
 * simply yields no data that interval (host interpolates).
 * Fixes vs Rev84:
 *   - EP_SUBMIT return checked: sync failure clears fb_inflight so the
 *     next poll retries instead of wedging forever with inflight=true.
 *   - Pending -> HW copy at submit time only (never touches in-flight buf).
 */
void uac2_feedback_poll(void)
{
#if (UAC2_FB_HW_ENABLE >= 2)
  /* Rev82 single-shot experiment: submit exactly ONE feedback transfer
   * per boot, then go silent. Distinguishes per-transfer toxicity
   * (board dies on the first IN completion => DCD DMA/desc pathology)
   * from repetition toxicity (survives one, dies on stream => IRQ pacing
   * storm). Host-side proof via usbmon EP1-IN transaction.
   */
#if UAC2_FB_SINGLE_SHOT
  static bool s_fb_shot = false;
  if (s_fb_shot)
    {
      return;
    }
#endif
  if (g_uac2_dev.is_streaming && g_uac2_dev.ep_fb && g_uac2_dev.fbreq)
    {
      /* Rev87e: ALWAYS refresh the HW buf with fresh PI output, even
       * while a transfer is in flight. Rationale (E2b/87d proven):
       * the CXD56 UDC auto-unloads isoc-IN polls from the FIFO with
       * (almost) no CPU involvement: no XFERDONE, no ISO_IN_DONE, no
       * completion ever (FBSTAT ok:1 done:0 stuck; wire shows the one
       * submitted value repeated HZZZ at ~828 polls/s while the device
       * sees ~3 IN-irqs/s). The only proven carrier of a NEW value to
       * the FIFO is the ~3/s IN-irq -> wrrequest -> DMA cycle, which
       * re-DMAs fbreq->buf. So the buf must track the PI output
       * continuously; the submit stays single-shot (re-submit would
       * double-queue a never-completing request: Rev76 wedge lesson).
       * A torn 4B write racing the DMA is harmless (host interpolates,
       * loop tau=4s). Fresh PI output only (pending flag).
       */
      if (g_uac2_dev.fb_has_pending)
        {
          uint32_t q = g_uac2_dev.fb_pending_q16;
          g_uac2_dev.fb_has_pending = false;
          g_uac2_dev.fbreq->buf[0] = (uint8_t)(q);
          g_uac2_dev.fbreq->buf[1] = (uint8_t)(q >> 8);
          g_uac2_dev.fbreq->buf[2] = (uint8_t)(q >> 16);
          g_uac2_dev.fbreq->buf[3] = (uint8_t)(q >> 24);
          g_uac2_dev.fbreq->len = 4;
        }

      if (!g_uac2_dev.fb_inflight)
        {
          uint32_t q;

          /* Fresh PI output only: skip when the pump produced nothing
           * new since the last submit (avoids re-sending a stale value).
           * NOTE Rev87e: with buf refresh above, has_pending is usually
           * false here; the submit below then re-sends the current buf.
           */
          q = (uint32_t)g_uac2_dev.fbreq->buf[0] |
              ((uint32_t)g_uac2_dev.fbreq->buf[1] << 8) |
              ((uint32_t)g_uac2_dev.fbreq->buf[2] << 16) |
              ((uint32_t)g_uac2_dev.fbreq->buf[3] << 24);
          g_uac2_dev.fb_inflight = true;
#if UAC2_FB_SINGLE_SHOT
      s_fb_shot = true;
      UAC2_TPRINTF("[UAC2] FB single-shot submit (4B Q16.16)\n");
#endif
      {
        int ret = EP_SUBMIT(g_uac2_dev.ep_fb, g_uac2_dev.fbreq);
        if (ret < 0)
          {
            /* Sync failure: no completion will ever arrive, so release
             * the slot immediately and re-stage for retry next poll.
             * Rate-limited log (task context, error path only).
             */
            g_uac2_dev.fb_inflight = false;
            g_uac2_dev.fb_pending_q16 = q;
            g_uac2_dev.fb_has_pending = true;
            g_uac2_dev.fb_submit_fail++;
            if (g_uac2_dev.fb_submit_fail <= 3 ||
                (g_uac2_dev.fb_submit_fail % 1000) == 0)
              {
                UAC2_TPRINTF("[UAC2] FB submit failed %lu (ret=%d)\n",
                       (unsigned long)g_uac2_dev.fb_submit_fail, ret);
              }
          }
        else
          {
            g_uac2_dev.fb_last_sent_q16 = q;
            g_uac2_dev.fb_submit_ok++;
            /* Rev86e: unconditional CNAK after arming data. The DCD only
             * CNAKs on submit when txwait is already set; with NAK stuck
             * from an earlier disable/SNAK and txwait clear, tokens stay
             * silent-NAK'd forever. Data is armed => NAK must go.
             */
            *(volatile uint32_t *)0x4E200020UL |= (1u << 8);
          }
      }
      }
    }
#else
  /* Bring-up ladder < 2: never submit (ISO IN submit wedges this DCD).
   * PI still computes (telemetry only).
   */
  (void)0;
#endif
}

/* Helpers: endpoint configure descriptors built from static table values */

static void uac2_build_iso_out_desc(FAR struct usb_epdesc_s *epdesc, uint8_t alt)
{
  uint16_t pktsz = uac2_alt_packet_size(alt);

  epdesc->len  = USB_SIZEOF_EPDESC;
  epdesc->type = USB_DESC_TYPE_ENDPOINT;
  epdesc->addr = UAC2_EP_ISO_OUT;
#if UAC2_SYNC_ADAPTIVE
  epdesc->attr = 0x09; /* Isochronous, Adaptive */
#else
  epdesc->attr = 0x05; /* Isochronous, Asynchronous */
#endif
  epdesc->mxpacketsize[0] = (uint8_t)(pktsz & 0xFF);
  epdesc->mxpacketsize[1] = (uint8_t)(pktsz >> 8);
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
  epdesc->interval = 0x04; /* 2^3 uframes = 1ms (Rev76: PI cadence match) */
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

  priv->fb_inflight = false;
  /* Rev85: drop staged value (stale PI across config change); keep
   * submit/complete counters for cross-config diagnosis. */
  priv->fb_has_pending = false;
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

#if UAC2_SINGLE_ALT0_STREAMING
  /* Rev78: record ONLY. All hardware bring-up (EP_CONFIGURE, CSR arm,
   * pre-submit, audio_start) is deferred to uac2_driver_poll() task
   * context. Doing it here (USB ISR context) wedges SMP deterministically
   * and silently neuters the DMA engine start even when it survives:
   * UP only ever worked because START happened to run via the deferred
   * start_pending tail (task context).
   */
  priv->is_streaming = true;
  priv->alt_setting  = 0;
#else
  /* Multi-alt legacy: alt bring-up already deferred; just stage Alt-1. */
  priv->is_streaming = true;
  priv->alt_setting  = 1;
#endif

  priv->config = config;
  return 0;
}

/* Task-context config bring-up (SMP-safe). Called from uac2_driver_poll()
 * when priv->config != priv->config_applied. Performs the hardware work
 * that uac2_setconfig used to do inline in USB ISR context:
 * EP_CONFIGURE ×2, EP2 CSR arm + CSR_DONE latch, EP2 OUT pre-submit,
 * and AUDIOIOC_START via uac2_audio_start().
 */

/* Rev84 (2): direct-MMIO isolation. All raw CXD5602 USB register pokes
 * live in uac2_hw_* helpers (mechanism); call sites keep policy (values).
 * Long-term these belong in the DCD (cxd56_usbdev.c via our ISOC patch),
 * but the DCD is SDK-owned, so isolation here is the pragmatic step.
 * USBDEV_BASE = 0x4E200000 (cxd5602_memorymap.h: ADSP_BASE + 0x220000).
 */
#define UAC2_HW_USBDEV_BASE   0x4E200000UL
#define UAC2_HW_USB_BUSY      (UAC2_HW_USBDEV_BASE + 0x808UL)
#define UAC2_HW_DEVCTL        (UAC2_HW_USBDEV_BASE + 0x404UL)
#define UAC2_HW_UDC_EP(n)     (UAC2_HW_USBDEV_BASE + 0x504UL + ((n) * 4u))
#define UAC2_HW_CSR_DONE_BIT  (1u << 13)

static void uac2_hw_arm_ep_csr(unsigned ep_no, uint32_t val_csr)
{
  volatile uint32_t *usb_busy =
    (volatile uint32_t *)UAC2_HW_USB_BUSY;
  volatile uint32_t *reg_csr =
    (volatile uint32_t *)UAC2_HW_UDC_EP(ep_no);
  volatile uint32_t *reg_devctl =
    (volatile uint32_t *)UAC2_HW_DEVCTL;

  while (*usb_busy);
  *reg_csr = val_csr;
  while (*usb_busy);

  /* Latch CSR into hardware with CSR_DONE (do NOT touch CSR_PRG!) */
  *reg_devctl = *reg_devctl | UAC2_HW_CSR_DONE_BIT;
  while (*usb_busy);
}

static void uac2_apply_config_task(Uac2Driver *priv)
{
  printf("[UAC2] applying config %u (was %u)\n",
         (unsigned)priv->config, (unsigned)priv->config_applied);
  fflush(stdout);

  /* Configure streaming endpoints so the controller hardware arms
   * before the host sends SET_INTERFACE / ISO traffic.
   */
  if (priv->ep_out)
    {
      struct usb_epdesc_s epdesc;
#if UAC2_SINGLE_ALT0_STREAMING
      uac2_build_iso_out_desc(&epdesc, 0);
#else
      uac2_build_iso_out_desc(&epdesc, 1);
#endif
      int ret = EP_CONFIGURE(priv->ep_out, &epdesc, false);
      printf("[UAC2] EP2 OUT configure -> %d\n", ret);
      fflush(stdout);
    }

#if !UAC2_SYNC_ADAPTIVE && (UAC2_FB_HW_ENABLE >= 1)
  if (priv->ep_fb)
    {
      struct usb_epdesc_s epdesc;
      uac2_build_fb_in_desc(&epdesc);
      int ret = EP_CONFIGURE(priv->ep_fb, &epdesc, false);
      printf("[UAC2] EP1 IN configure -> %d\n", ret);
      fflush(stdout);
      /* Rev86 (silicon manual 3.18.10.2.1 + CSR matching): EP_CONFIGURE
       * programs UDC slot 1 with the STALE intf/alt latched in STATUS at
       * config time (0/0), so IN tokens for the streaming interface (1/0)
       * never reach EP1's logic: no IN ISR, no DMA, pristine descriptor,
       * empty (len 0) completions on the wire. EP2-OUT only works because
       * it is manually re-armed below with intf=1. Do the same for EP1:
       * slot = EP1 IN + ISOC + cfg1 + intf1 + alt0 + maxpacket 4.
       */
      uac2_hw_arm_ep_csr(1, (1u | (1u << 4) | (1u << 5) | (1u << 7) |
                             (1u << 11) | (0u << 15) | (4u << 19)));
      printf("[UAC2] EP1 CSR armed (0x508) for intf=1/alt=0\n");
      fflush(stdout);
      /* Rev86d experiment (MAXPKTSIZE live poke) REMOVED: the value now
       * comes from the DCD table (CXD56_UAC2FBMAXPACKET) programmed at
       * hw-init. Single source of truth; live pokes mid-stream confuse
       * the IP.
       */
    }
#endif

  /* Rev84 (2): CSR value computation stays here (policy); the raw
   * register poke lives in uac2_hw_arm_ep_csr() (mechanism).
   */
#if UAC2_SINGLE_ALT0_STREAMING
  uint32_t val_csr = (2u | (0u << 4) | (1u << 5) | (1u << 7) |
                      (1u << 11) | (0u << 15) | (200u << 19)); /* 0x064008A2 */
#else
  uint32_t val_csr = (2u | (0u << 4) | (1u << 5) | (1u << 7) |
                      (1u << 11) | (1u << 15) | (100u << 19)); /* 0x032088A2 */
#endif

  /* Arm EP2 OUT CSR slot (0x4E20050C = CXD56_USB_DEV_UDC_EP2) explicitly */
  uac2_hw_arm_ep_csr(2, val_csr);
  printf("[UAC2] EP2 CSR armed (0x50C) <- 0x%08lx\n", (unsigned long)val_csr);
  fflush(stdout);

#if UAC2_SINGLE_ALT0_STREAMING
  /* Arm EP2 OUT for streaming so it is ready for incoming audio */
  priv->alt_applied = 0;
  if (priv->ep_out && priv->outreq)
    {
      priv->outreq->len = UAC2_PACKET_SIZE_24BIT_192K;
      EP_SUBMIT(priv->ep_out, priv->outreq);
      printf("[UAC2] EP2 OUT streaming armed (len=%u)\n",
             (unsigned)priv->outreq->len);
      fflush(stdout);
    }
#if !UAC2_SYNC_ADAPTIVE && (UAC2_FB_HW_ENABLE >= 2)
  /* Rev76 Alt-0 async: NO submit here. The first and all later submits
   * are paced (<=1kHz) from the pump thread via uac2_feedback_poll(),
   * which starts flowing within ~2ms of streaming start.
   */
#endif
  uac2_audio_start();
#endif

  priv->config_applied = priv->config;
}

static int uac2_setinterface(Uac2Driver *priv, uint8_t ifno, uint8_t alt)
{
  volatile uint32_t *reg_devcfg = (volatile uint32_t *)0x4E200400UL;
  volatile uint32_t *reg_devctl = (volatile uint32_t *)0x4E200404UL;
  volatile uint32_t *reg_devsts = (volatile uint32_t *)0x4E200408UL;

  /* EP2 OUT control register: 0x4E200200 + 2*0x20 = 0x4E200240 */
  volatile uint32_t *reg_ep2_out_ctl = (volatile uint32_t *)0x4E200240UL;
#if !UAC2_SINGLE_ALT0_STREAMING
  /* EP1 IN control register:  0x4E200000 + 1*0x20 = 0x4E200020 */
  volatile uint32_t *reg_ep1_in_ctl  = (volatile uint32_t *)0x4E200020UL;
#endif

  UAC2_TPRINTF("[UAC2] SET_INTERFACE if=%u alt=%u | CFG=0x%08lx CTL=0x%08lx STS=0x%08lx | EP2CTL=0x%08lx\n",
         ifno, alt, (unsigned long)*reg_devcfg, (unsigned long)*reg_devctl, (unsigned long)*reg_devsts,
         (unsigned long)*reg_ep2_out_ctl);

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

#if UAC2_SINGLE_ALT0_STREAMING
  if (alt != 0)
    {
      return -EINVAL;
    }

  /* Alt 0 is the single streaming interface: keep active, clear STALL & NAK */
  priv->is_streaming = true;
  priv->alt_setting  = 0;

#define CXD56_HW_USB_STALL (1u << 0)
#define CXD56_HW_USB_CNAK  (1u << 8)

  *reg_ep2_out_ctl = (*reg_ep2_out_ctl & ~CXD56_HW_USB_STALL) | CXD56_HW_USB_CNAK;

  return 0;
#else
  if (alt > 2)
    {
      return -EINVAL;
    }

  if (alt == priv->alt_setting && ((alt > 0) == priv->is_streaming))
    {
      return 0;
    }

  if (alt == 0)
    {
      /* Stop streaming request (ISR context: flags + SNAK only, NO STALL!).
       * Note: NuttX EP_DISABLE() calls cxd56_epstall(false) which sets USB_STALL,
       * causing hardware to STALL subsequent SET_INTERFACE(alt>0).
       * Follow Linux pch_udc_ep_disable: set SNAK and clear STALL!
       */
      priv->is_streaming = false;
      priv->alt_setting  = 0;

#define CXD56_HW_USB_STALL (1u << 0)
#define CXD56_HW_USB_SNAK  (1u << 7)

      *reg_ep2_out_ctl = (*reg_ep2_out_ctl & ~CXD56_HW_USB_STALL) | CXD56_HW_USB_SNAK;
      *reg_ep1_in_ctl  = (*reg_ep1_in_ctl  & ~CXD56_HW_USB_STALL) | CXD56_HW_USB_SNAK;

      return 0;
    }

  /* Alt-1 (16-bit PCM) / Alt-2 (24-bit PCM in 32-bit slot): record request.
   * Hardware bring-up (EP_CONFIGURE/SUBMIT, audio_start, logging) runs
   * deferred in uac2_driver_poll() task context: EP0 setup executes in USB
   * interrupt context where printf/malloc/ISOC bring-up hung the MCU.
   */
  priv->alt_setting  = alt;
  priv->is_streaming = true;

  return 0;
#endif
}

/**
 * @brief Apply pending Alt changes in task context (call every ~100ms).
 *
 * Performs the deferred streaming hardware bring-up with stepwise logging
 * so a hang localizes precisely (last printed step == killer).
 */
void uac2_driver_poll(void)
{
  Uac2Driver *priv = &g_uac2_dev;
  uint8_t alt;
  /* Rev86c-diag (temporary): EP1 IRQ accounting every ~5s while streaming. */
  static uint32_t s_poll_ticks = 0;

  if (!priv->dev)
    {
      return;
    }

  if ((++s_poll_ticks % 50) == 0 && priv->is_streaming)
    {
      printf("[UAC2] EP1IRQ in:%lu xfer:%lu iso:%lu bna:%lu he:%lu txe:%lu tdc:%lu\n",
             (unsigned long)g_ep1_irq_cnt[0],
             (unsigned long)g_ep1_irq_cnt[1],
             (unsigned long)g_ep1_irq_cnt[2],
             (unsigned long)g_ep1_irq_cnt[3],
             (unsigned long)g_ep1_irq_cnt[4],
             (unsigned long)g_ep1_irq_cnt[5],
             (unsigned long)g_ep1_irq_cnt[6]);
      fflush(stdout);
    }

  /* Config bring-up first, always in task context (Rev78 SMP fix).
   * Covers the single-Alt-0 path: EP configure, CSR arm, pre-submit,
   * audio start. uac2_setconfig (ISR) only stages flags. */
  if (priv->config != priv->config_applied)
    {
      if (priv->config == 0 || priv->config != UAC2_CONFIG_ID)
        {
          uac2_audio_stop();
          priv->config_applied = 0;
          priv->alt_applied = priv->alt_setting;
          return;
        }

      uac2_apply_config_task(priv);
    }

  if (priv->alt_setting == priv->alt_applied)
    {
      return;
    }

  alt = priv->alt_setting;
  printf("[UAC2] applying alt %u (was %u)\n", (unsigned)alt,
         (unsigned)priv->alt_applied);
  fflush(stdout);

  if (alt == 0)
    {
      uac2_audio_stop();
      priv->alt_applied = 0;
      return;
    }

  if (priv->ep_out)
    {
      struct usb_epdesc_s epdesc;
      uac2_build_iso_out_desc(&epdesc, alt);
      printf("[UAC2] ep_out configure (alt %u, pkt %u)\n",
             (unsigned)alt, (unsigned)uac2_alt_packet_size(alt));
      fflush(stdout);
      EP_CONFIGURE(priv->ep_out, &epdesc, false);
      printf("[UAC2] ep_out configured\n");
      fflush(stdout);

      if (priv->outreq)
        {
          priv->outreq->len = uac2_alt_packet_size(alt);
          EP_SUBMIT(priv->ep_out, priv->outreq);
          printf("[UAC2] ep_out streaming armed\n");
          fflush(stdout);
        }
      else
        {
          printf("[UAC2] ep_out: no request (bind alloc failed), ACK-only\n");
          fflush(stdout);
        }
    }
  else
    {
      printf("[UAC2] ep_out unavailable, ACK-only streaming\n");
      fflush(stdout);
    }

  if (priv->ep_fb)
    {
      struct usb_epdesc_s epdesc;
      uac2_build_fb_in_desc(&epdesc);
      EP_CONFIGURE(priv->ep_fb, &epdesc, true);

#if (UAC2_FB_HW_ENABLE >= 2)
      /* Rev85: same guards as uac2_feedback_poll() (Rev76 wedge lesson
       * + Rev83 stuck-IN finding). Never touch HW buf while inflight;
       * check EP_SUBMIT return so a sync failure retries next stream.
       */
      if (priv->fbreq && !priv->fb_inflight)
        {
          if (priv->fb_has_pending)
            {
              uint32_t q = priv->fb_pending_q16;
              priv->fb_has_pending = false;
              priv->fbreq->buf[0] = (uint8_t)(q);
              priv->fbreq->buf[1] = (uint8_t)(q >> 8);
              priv->fbreq->buf[2] = (uint8_t)(q >> 16);
              priv->fbreq->buf[3] = (uint8_t)(q >> 24);
              priv->fb_last_sent_q16 = q;
            }
          /* Else: keep bind-time nominal already in HW buf. */
          priv->fbreq->len = 4;
          priv->fb_inflight = true;
          {
            int fbret = EP_SUBMIT(priv->ep_fb, priv->fbreq);
            if (fbret < 0)
              {
                priv->fb_inflight = false;
                priv->fb_has_pending = true;
                priv->fb_pending_q16 = priv->fb_last_sent_q16;
                priv->fb_submit_fail++;
              }
            else
              {
                priv->fb_submit_ok++;
              }
          }
        }
#endif
    }

  uac2_audio_start();
  priv->alt_applied = alt;
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
  priv->config       = 0;
  priv->config_applied = 0;
  priv->sample_rate  = UAC2_SAMPLE_RATE_192K;
  priv->alt_setting  = 0;
  priv->alt_applied  = 0;
  priv->is_streaming = false;
  memset(priv->mute, 0, sizeof(priv->mute));
  memset(priv->volume, 0, sizeof(priv->volume));
  priv->outreq = NULL;
  priv->fbreq  = NULL;
  priv->fb_inflight = false;
  /* Rev85 double-buffer init: nominal staged nowhere yet; HW buf holds
   * nominal until the first PI update stages a fresh value. */
  priv->fb_pending_q16 = (24u << 16);
  priv->fb_has_pending = false;
  priv->fb_last_sent_q16 = (24u << 16);
  priv->fb_submit_ok = 0;
  priv->fb_submit_fail = 0;
  priv->fb_complete_cnt = 0;

  /* Pre-allocate streaming requests here (task context): allocreq must
   * never run in EP0 setup (USB interrupt) context.
   */
  if (priv->ep_out)
    {
      priv->outreq = usbdev_allocreq(priv->ep_out, UAC2_ISO_OUT_MAXPKT);
      if (priv->outreq)
        {
          priv->outreq->callback = uac2_iso_out_complete;
        }
    }

  if (priv->ep_fb)
    {
      priv->fbreq = usbdev_allocreq(priv->ep_fb, 4);
      if (priv->fbreq)
        {
          priv->fbreq->callback = uac2_fb_in_complete;
          /* Nominal feedback payload for 192kHz HS: Fs/8000 = 24.0
           * in 16.16 fixed point (0x00180000 LE).
           */
          priv->fbreq->buf[0] = 0x00;
          priv->fbreq->buf[1] = 0x00;
          priv->fbreq->buf[2] = 0x18;
          priv->fbreq->buf[3] = 0x00;
          priv->fbreq->len    = 4;
        }
    }

  UAC2_TPRINTF("[UAC2] bind EPs: out=%p outreq=%p fb=%p fbreq=%p\n",
         (void *)priv->ep_out, (void *)priv->outreq,
         (void *)priv->ep_fb, (void *)priv->fbreq);

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
              /* TEMP-DIAG Rev54: 非対応値もSTALLせず192kクランプACK。
               * SET_CUR拒否STALL後のEP0リカバリ不全による焼き付き疑いの
               * 切り分け。エンジンは192k固定のため実害なし（のはず）。
               * RANGEは192k-only維持。
               */
              if (dataout && outlen >= 4)
                {
                  uint32_t newfreq;
                  memcpy(&newfreq, dataout, 4);
                  if (newfreq == 192000)
                    {
                      priv->sample_rate = newfreq;
                    }
                    else
                      {
                        UAC2_TPRINTF("[UAC2] SET_CUR freq %lu clamped to 192000 (no STALL)\n",
                               (unsigned long)newfreq);
                        priv->sample_rate = 192000;
                      }
                }

              return 0;
            }
        }
      else if (req == UAC2_CS_RANGE)
        {
          /* Single discrete subrange: 192kHz native only.
           * (Former 6-rate table was for Windows usbaudio2 compat;
           * Windows path abandoned (CXD5602 Alt>0 HW stall), Linux-only now.
           * Advertising other rates lets hosts (e.g. PipeWire 48k) open
           * the device at a rate the fixed engine cannot serve.)
           * One discrete rate: dMIN == dMAX == freq, dRES == 0
           * (UAC2 spec ADC-2 Table 5-1 / Sec 5.2.1).
           */
          static const uint8_t range_desc[] =
          {
            0x01, 0x00,               /* wNumSubRanges: 1 */

            /* Subrange 1: 192000 Hz (0x0002EE00, native) */
            0x00, 0xEE, 0x02, 0x00,   /* dMIN: 192000 */
            0x00, 0xEE, 0x02, 0x00,   /* dMAX: 192000 */
            0x00, 0x00, 0x00, 0x00    /* dRES: 0 (discrete) */
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
                  priv->mute[0] = (dataout[0] != 0);
                  priv->mute[1] = priv->mute[0];
                  priv->mute[2] = priv->mute[0];
                  uac2_audio_set_mute(priv->mute[0]);
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
                  int16_t raw_vol;
                  uint8_t percent;

                  memcpy(&priv->volume[0], dataout, 2);
                  priv->volume[1] = priv->volume[0];
                  priv->volume[2] = priv->volume[0];

                  raw_vol = (int16_t)priv->volume[0];
                  if (raw_vol <= -24576) /* -96dB */
                    {
                      percent = 0;
                    }
                  else if (raw_vol >= 0) /* 0dB */
                    {
                      percent = 100;
                    }
                  else
                    {
                      percent = (uint8_t)(((int32_t)(raw_vol + 24576) * 100) / 24576);
                    }

                  uac2_audio_set_volume(percent);
                }

              return 0;
            }
        }
      else if (req == UAC2_CS_RANGE)
        {
          /* One subrange: MIN -96dB (0xA000), MAX 0dB, RES 1dB (0x0100) */
          static const uint8_t vol_range[] =
          {
            0x01, 0x00,   /* wNumSubRanges: 1 */
            0x00, 0xA0,   /* MIN Lo (-96dB LE: 0xA000) */
            0x00, 0x00,   /* MAX Lo (0dB) */
            0x00, 0x01    /* RES Lo (1dB) */
          };

          memcpy(ctrlreq->buf, vol_range, sizeof(vol_range));
          return sizeof(vol_range);
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

              case USB_DESC_TYPE_BOS:
                /* MS OS 2.0 entry point (needs bcdUSB >= 0x0201).
                 * Linux ignores the unknown platform capability.
                 */
                memcpy(ctrlreq->buf, g_uac2_bos_desc, g_uac2_bos_desc_len);
                ret = g_uac2_bos_desc_len;
                break;

              default:
                break;
              }
          }
          break;

        case USB_REQ_SETCONFIGURATION:
          if (type == 0)
            {
              /* Log negotiated USB speed: 2=FULL (12Mbps, 192k impossible),
               * 3=HIGH (480Mbps, required for 192k/24bit). */
              UAC2_TPRINTF("[UAC2] SET_CONFIG %u, USB speed=%u (%s)\n",
                     (unsigned)value, (unsigned)dev->speed,
                     (dev->speed == 3) ? "HIGH-480M" :
                     (dev->speed == 2) ? "FULL-12M" : "OTHER");
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
              /* Rev19 diag: log every SET_IF arrival. The CXD56 DCD calls
               * CLASS_SETUP twice per SET_IF: once from EP0 SETUP (real
               * packet values, dataout != NULL) and once from the USB_INT_SI
               * ISR with HW-synthesized values from USB_DEV_STATUS
               * (dataout == NULL). A synthesized alt > 2 returns -EINVAL
               * and STALLs EP0 after the real path already ACKed.
               */
              UAC2_TPRINTF("[UAC2] SET_IF if=%u alt=%u via=%s\n",
                     (unsigned)index, (unsigned)value,
                     dataout ? "ep0" : "SI-synth");
              ret = uac2_setinterface(priv, (uint8_t)index, (uint8_t)value);
              UAC2_TPRINTF("[UAC2] SET_IF -> %d\n", ret);
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
      else if (entity_id == 0)
        {
          /* Interface-level requests (Entity ID 0) */
          uint8_t ifno = (uint8_t)(index & 0xFF);
          if (ifno == UAC2_IF_AUDIO_CONTROL)
            {
              /* Latency Control (CS=0x01) or general AC requests */
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
          else if (ifno == UAC2_IF_AUDIO_STREAMING)
            {
              if (cs == 0x01) /* AS_ACT_ALT_SETTING_CONTROL */
                {
                  if (req == UAC2_CS_CUR && (type & USB_REQ_DIR_IN))
                    {
                      ctrlreq->buf[0] = priv->alt_setting;
                      ret = 1;
                    }
                  else if (req == UAC2_CS_CUR)
                    {
                      ret = 0;
                    }
                }
              else if (cs == 0x02) /* AS_VAL_ALT_SETTINGS_CONTROL */
                {
                  if (req == UAC2_CS_CUR && (type & USB_REQ_DIR_IN))
                    {
#if UAC2_SINGLE_ALT0_STREAMING
                      /* Only Alt 0 is valid: bit 0 = 0x01 */
                      ctrlreq->buf[0] = 0x01;
#else
                      /* Alt 0 (zero bandwidth), Alt 1 (16-bit PCM) and
                       * Alt 2 (24-bit PCM) are valid: bits 0, 1, 2 = 0x07 */
                      ctrlreq->buf[0] = 0x07;
#endif
                      ret = 1;
                    }
                }
            }
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
  /* MS vendor requests (WinUSB auto-bind; ISR context: memcpy only) */
  else if ((type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_VENDOR)
    {
      if (req == UAC2_MS_VENDOR_CODE && index == UAC2_MSOS20_INDEX)
        {
          memcpy(ctrlreq->buf, g_uac2_msos20_set, g_uac2_msos20_set_len);
          ret = g_uac2_msos20_set_len;
        }
    }

  /* Record to lock-free setup trace log */
  uac2_log_setup(type, req, value, index, len, (int16_t)ret);

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
  /* 実給電リング（g_pcm_ring）の統計を返す。旧g_uac2_dev.ringbufは
   * 死にバッファだったため参照しない（Claude指摘）。
   */
  uac2_audio_get_ring_stats(underrun, overrun, buffered);
}

void uac2_dump_setup_logs(void)
{
  while (g_log_tail != g_log_head)
    {
      Uac2SetupLog log = g_setup_logs[g_log_tail];
      g_log_tail = (g_log_tail + 1) % UAC2_LOG_SIZE;

      uint8_t entity = (uint8_t)(log.index >> 8);
      uint8_t ifno   = (uint8_t)(log.index & 0xFF);
      uint8_t cs     = (uint8_t)(log.value >> 8);
      uint8_t ch     = (uint8_t)(log.value & 0xFF);

      printf("[EP0] type=0x%02x req=0x%02x val=0x%04x(cs=%u,ch=%u) idx=0x%04x(ent=%u,if=%u) len=%u -> ret=%d\n",
             log.type, log.req, log.value, cs, ch, log.index, entity, ifno, log.len, log.ret);
    }
}

