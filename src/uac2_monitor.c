/**
 * @file uac2_monitor.c
 * @brief Low-priority telemetry thread (sub-core offload for non-audio work).
 *
 * All printf/fflush for periodic status lives here and ONLY here.
 * Main thread captures cheap snapshots; pump thread only bumps counters.
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <unistd.h>

#include "uac2.h"
#include "uac2_monitor.h"
#include "uac2_asmp.h"
#include "uac2_audio_dma.h"

/* Drained from monitor context (was main-loop context). Lock-free queue. */
extern void uac2_dump_setup_logs(void);

static struct uac2_mon_snapshot_s g_mon_slot;
static uint32_t g_mon_seq = 0;
static pthread_mutex_t g_mon_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_mon_sem;
static volatile bool g_mon_has_data = false;
static bool g_mon_inited = false;

/* ASMP sub-core formatter (phase 1). NULL = local formatting fallback. */
static volatile struct uac2_asmp_shm_s *g_asmp = NULL;
static uint32_t g_asmp_hb_last = 0;
static uint32_t g_asmp_stall = 0;

void uac2_pin_self_to_cpu(int cpu)
{
#ifdef CONFIG_SMP
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  /* 0 = calling thread. Best effort: ignore errors (e.g. CPU not present). */
  (void)sched_setaffinity(0, sizeof(cpu_set_t), &set);
#else
  (void)cpu;
#endif
}

static void uac2_mon_print(const struct uac2_mon_snapshot_s *s)
{
#if UAC2_SILENT_DIAG
  /* Silent bring-up: keep UART fully quiet except streaming state line.
   * Sub-core offload is still active (thread + affinity), just no prints. */
  (void)s;
  return;
#else
  const char *state_str = s->is_streaming ? "STREAMING (192kHz Active)" :
                          (s->alt > 0 ? "ALT_SETTING_ACTIVE" :
                           "STANDBY (Waiting Host Playback)");

  printf("[UAC2 #%lu] %s | Alt:%u | SR:%lu Hz | Buf:%lu B | Under:%lu | Over:%lu\n",
         (unsigned long)s->tick_no, state_str, (unsigned)s->alt,
         (unsigned long)s->sample_rate, (unsigned long)s->buffered,
         (unsigned long)s->underrun, (unsigned long)s->overrun);
  printf("[AUD #%lu] playing:%d fd:%d enq:%lu deq:%lu aud_udr:%lu rst:%lu freetop:%d clk:%d errcont:%lu\n",
         (unsigned long)s->tick_no, (int)s->aplaying, s->afd,
         (unsigned long)s->enq, (unsigned long)s->deq,
         (unsigned long)s->aud_udr, (unsigned long)s->rst, s->freetop,
         s->clk, (unsigned long)s->errcont);
  printf("[FEED #%lu] partial:%lu empty:%lu done:%lu erronly:%lu errdone:%lu dup:%lu gap:%lu fdup:%lu fgap:%lu svd:%lu svu:%lu fb:0x%08lx ferr:%ld\n",
         (unsigned long)s->tick_no,
         (unsigned long)s->partial, (unsigned long)s->empty,
         (unsigned long)s->done,
         (unsigned long)s->erronly,
         (unsigned long)s->errdone,
         (unsigned long)s->dup, (unsigned long)s->gap,
         (unsigned long)s->fdup, (unsigned long)s->fgap,
         (unsigned long)s->svd, (unsigned long)s->svu,
         (unsigned long)s->fbff, (long)s->fberr);
  printf("[EOGAP #%lu] g0:%lu g1:%lu g2:%lu g3:%lu g4:%lu g5:%lu g6:%lu g7:%lu g8:%lu g9:%lu\n",
         (unsigned long)s->tick_no,
         (unsigned long)s->eogap[0],
         (unsigned long)s->eogap[1],
         (unsigned long)s->eogap[2],
         (unsigned long)s->eogap[3],
         (unsigned long)s->eogap[4],
         (unsigned long)s->eogap[5],
         (unsigned long)s->eogap[6],
         (unsigned long)s->eogap[7],
         (unsigned long)s->eogap[8],
         (unsigned long)s->eogap[9]);
  printf("[DAT #%lu] dc:%lu sc:%lu raw:0x%08lx dst:0x%08lx crc:0x%08lx bytes:%lu isum:%lu icalls:%lu\n",
         (unsigned long)s->tick_no,
         (unsigned long)s->dc, (unsigned long)s->sc,
         (unsigned long)s->r_smp, (unsigned long)s->d_smp,
         (unsigned long)s->crc_val, (unsigned long)s->crc_bytes,
         (unsigned long)s->iso_sum_bytes, (unsigned long)s->iso_sum_calls);
  /* Pump health + offloaded event counters (replaces pump-thread printf). */
  printf("[MON #%lu] wakes:%lu newstream:%lu(left=%lu) fluke:%lu revived:%lu failed:%lu enqfail:%lu msg_udr:%lu msg_ioe:%lu\n",
         (unsigned long)s->tick_no,
         (unsigned long)s->pump_wakes,
         (unsigned long)s->mon_newstream,
         (unsigned long)s->mon_newstream_leftover,
         (unsigned long)s->mon_fluke_cancel,
         (unsigned long)s->mon_restart_revived,
         (unsigned long)s->mon_restart_failed,
         (unsigned long)s->mon_enq_fail,
         (unsigned long)s->mu, (unsigned long)s->me);

  if (s->has_reg)
    {
      printf("[REG] CFG=0x%08lx CTL=0x%08lx STS=0x%08lx INT=0x%08lx BUSY=0x%08lx | EP2CTL=0x%08lx EP2STS=0x%08lx\n",
             (unsigned long)s->reg_devcfg, (unsigned long)s->reg_devctl,
             (unsigned long)s->reg_devsts,
             (unsigned long)s->reg_devintr, (unsigned long)s->reg_busy,
             (unsigned long)s->reg_ep2ctl, (unsigned long)s->reg_ep2sts);
      printf("[AUDREG] ADR=0x%08lx SMPL=0x%08lx CMD=0x%08lx CHSEL=0x%08lx MON=0x%08lx ISTAT=0x%08lx\n",
             (unsigned long)s->au_adr, (unsigned long)s->au_smpls,
             (unsigned long)s->au_cmd, (unsigned long)s->au_chsel,
             (unsigned long)s->au_mon, (unsigned long)s->au_istat);
      if (s->has_errsnap)
        {
          printf("[ERRSNAP] intbit=0x%08lx stat=0x%08lx mask=0x%08lx mon=0x%08lx smpls=0x%08lx addr=0x%08lx state=%lu errcont=%lu\n",
                 (unsigned long)s->errsnap[0],
                 (unsigned long)s->errsnap[1],
                 (unsigned long)s->errsnap[2],
                 (unsigned long)s->errsnap[3],
                 (unsigned long)s->errsnap[4],
                 (unsigned long)s->errsnap[5],
                 (unsigned long)s->errsnap[6],
                 (unsigned long)s->errcont);
        }
      if (s->has_errsnap2)
        {
          printf("[ERRSNAP2] intbit=0x%08lx stat=0x%08lx mask=0x%08lx mon=0x%08lx smpls=0x%08lx addr=0x%08lx state=%lu errcont=%lu\n",
                 (unsigned long)s->errsnap2[0],
                 (unsigned long)s->errsnap2[1],
                 (unsigned long)s->errsnap2[2],
                 (unsigned long)s->errsnap2[3],
                 (unsigned long)s->errsnap2[4],
                 (unsigned long)s->errsnap2[5],
                 (unsigned long)s->errsnap2[6],
                 (unsigned long)s->errsnap2[7]);
        }
    }

  /* EP0 setup trace: drained here so main/USB path never touches UART. */
  uac2_dump_setup_logs();
  fflush(stdout);
#endif
}

/* Rev81-diag (一時): USB intakeキャプチャの変化分表示。
 * head=当ストリーム先頭64B, frozen=前ストリーム末尾64B。
 * MONスレッド専用 (UART可)。変化時のみ1行出す。 */
static void uac2_mon_drain_cap(void)
{
  static uint8_t last_head[64];
  static uint8_t last_frozen[64];
  static bool have_last = false;
  const uint8_t *hd;
  const uint8_t *fr;
  const uint8_t *rl;
  bool hv;
  bool fv;
  bool show_h;
  bool show_f;
  int i;

  uac2_audio_get_cap(&hd, &fr, &rl, &hv, &fv);
  (void)rl;
  show_h = (hv && (!have_last || memcmp(hd, last_head, sizeof(last_head)) != 0));
  show_f = (fv && (!have_last || memcmp(fr, last_frozen, sizeof(last_frozen)) != 0));
  if (!show_h && !show_f)
    {
      return;
    }

  printf("[CAP]");
  if (hv)
    {
      printf(" head:");
      for (i = 0; i < 64; i++)
        {
          printf("%02x", hd[i]);
        }
      memcpy(last_head, hd, sizeof(last_head));
    }
  if (fv)
    {
      printf(" frozen:");
      for (i = 0; i < 64; i++)
        {
          printf("%02x", fr[i]);
        }
      memcpy(last_frozen, fr, sizeof(last_frozen));
    }
  printf("\n");
  have_last = true;
  fflush(stdout);
}

static void *uac2_monitor_thread(void *arg)
{
  (void)arg;

  /* Sub-core offload: park the logging thread away from the audio CPU. */
  uac2_pin_self_to_cpu(UAC2_MON_CPU);

  for (;;)
    {
      sem_wait(&g_mon_sem);

      struct uac2_mon_snapshot_s snap;
      uint32_t seq;
      pthread_mutex_lock(&g_mon_lock);
      snap = g_mon_slot;
      seq = g_mon_seq;
      g_mon_has_data = false;
      pthread_mutex_unlock(&g_mon_lock);

      /* Sub-core path: worker formatted this snapshot -> dump its text.
       * Wait briefly (worker formats in ms); fall back to local format
       * on timeout or worker stall (3-strike heartbeat check). */
      bool served = false;
      if (g_asmp != NULL)
        {
          for (int i = 0; i < 50; i++)
            {
              if (g_asmp->inbox_done == seq)
                {
                  break;
                }
              usleep(1000);
            }
          if (g_asmp->inbox_done == seq)
            {
              /* Drain worker outbox ring to UART (no formatting on main). */
              uint32_t head = g_asmp->out_head;
              uint32_t tail = g_asmp->out_tail;
              while (tail != head)
                {
                  uint32_t chunk = (head > tail) ? (head - tail)
                                                 : (UAC2_ASMP_OUTBOX_SIZE - tail);
                  fwrite((const char *)g_asmp->outbox + tail, 1, chunk,
                         stdout);
                  tail = (head > tail) ? head : 0;
                  g_asmp->out_tail = tail;
                  head = g_asmp->out_head;
                }
              served = true;
            }
          /* Heartbeat stall detection (worker crash/hang). */
          if (g_asmp->worker_hb != g_asmp_hb_last)
            {
              g_asmp_hb_last = g_asmp->worker_hb;
              g_asmp_stall = 0;
            }
          else if (++g_asmp_stall >= 3)
            {
              printf("[UAC2-MON] sub-core stall detected, local fallback\n");
              g_asmp = NULL;
            }
        }

      if (!served)
        {
          uac2_mon_print(&snap);
        }
      else
        {
#if !UAC2_SILENT_DIAG
          /* EP0 setup trace still drains on main (main-core memory). */
          uac2_dump_setup_logs();
          fflush(stdout);
#endif
        }
#if !UAC2_SILENT_DIAG
      /* Rev81-diag (一時): intakeキャプチャ変化分のみ表示 (ASMP有無どちらの経路でも)。 */
      uac2_mon_drain_cap();
#endif
    }

  return NULL;
}

int uac2_monitor_init(void)
{
  if (g_mon_inited)
    {
      return 0;
    }

  memset(&g_mon_slot, 0, sizeof(g_mon_slot));
  sem_init(&g_mon_sem, 0, 0);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, UAC2_MON_STACKSIZE);
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = UAC2_MON_PRIORITY;
  pthread_attr_setschedparam(&attr, &sp);

  pthread_t tid;
  int ret = pthread_create(&tid, &attr, uac2_monitor_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return ret;
    }

  pthread_detach(tid);
  g_mon_inited = true;

#ifdef CONFIG_SMP
  printf("[UAC2-MON] telemetry offloaded: prio=%d cpu=%d (SMP)\n",
         UAC2_MON_PRIORITY, UAC2_MON_CPU);
#else
  printf("[UAC2-MON] telemetry isolated: prio=%d (single-core build, no affinity)\n",
         UAC2_MON_PRIORITY);
#endif
  fflush(stdout);

  /* ASMP sub-core formatter (phase 1). Failure falls back to local
   * formatting above; audio is unaffected either way. */
  {
    volatile struct uac2_asmp_shm_s *shm = NULL;
    if (uac2_asmp_boot(&shm) == 0 && shm != NULL)
      {
        g_asmp = shm;
        g_asmp_hb_last = shm->worker_hb;
        printf("[UAC2-MON] sub-core formatter active\n");
      }
    else
      {
        printf("[UAC2-MON] sub-core unavailable, local formatting\n");
      }
    fflush(stdout);
  }
  return 0;
}

void uac2_monitor_push(const struct uac2_mon_snapshot_s *snap)
{
  if (!g_mon_inited || snap == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mon_lock);
  g_mon_slot = *snap;
  g_mon_seq++;
  /* Hand to sub-core (SPSC: payload, barrier, then seq). */
  if (g_asmp != NULL)
    {
      memcpy((void *)&g_asmp->inbox, snap, sizeof(*snap));
      __sync_synchronize();
      g_asmp->inbox_seq = g_mon_seq;
    }
  g_mon_has_data = true;
  pthread_mutex_unlock(&g_mon_lock);

  /* Coalesce: if monitor lags, it prints the latest snapshot only. */
  int sval = 0;
  sem_getvalue(&g_mon_sem, &sval);
  if (sval <= 0)
    {
      sem_post(&g_mon_sem);
    }
}
