/**
 * @file monw.c
 * @brief ASMP sub-core 1 worker: telemetry formatter for UAC2 DAC.
 *
 * Dependency-free (no worker libc): hand-rolled integer emitters only.
 * Attaches the shared segment, ABI-gates it, then formats every inbox
 * snapshot into the outbox text ring for the main core to UART-dump.
 */

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <asmp/mpshm.h>
#include <asmp/mpmutex.h>

#include "uac2_asmp.h"

/* Idle backoff via nuttx/arch.h up_udelay (same as HexaMIDI workers). */
#include <nuttx/arch.h>

/* ---- tiny emitters (no libc) -------------------------------------- */

struct emit_s
{
  char *p;
  char *end;
};

static void emit_c(struct emit_s *e, char c)
{
  if (e->p < e->end)
    {
      *e->p++ = c;
    }
}

static void emit_s(struct emit_s *e, const char *s)
{
  while (*s)
    {
      emit_c(e, *s++);
    }
}

static void emit_u32(struct emit_s *e, uint32_t v)
{
  char tmp[10];
  int n = 0;
  if (v == 0)
    {
      emit_c(e, '0');
      return;
    }
  while (v > 0 && n < 10)
    {
      tmp[n++] = (char)('0' + (v % 10u));
      v /= 10u;
    }
  while (n > 0)
    {
      emit_c(e, tmp[--n]);
    }
}

static void emit_i32(struct emit_s *e, int32_t v)
{
  if (v < 0)
    {
      emit_c(e, '-');
      /* INT_MIN-safe negate via unsigned */
      uint32_t u = (uint32_t)(-(v + 1)) + 1u;
      emit_u32(e, u);
    }
  else
    {
      emit_u32(e, (uint32_t)v);
    }
}

static void emit_hex8(struct emit_s *e, uint32_t v)
{
  int i;
  for (i = 7; i >= 0; i--)
    {
      uint32_t nib = (v >> (i * 4)) & 0xFu;
      emit_c(e, (char)(nib < 10u ? '0' + nib : 'a' + (nib - 10u)));
    }
}

/* ---- outbox commit -------------------------------------------------- */

static void outbox_write(volatile struct uac2_asmp_shm_s *shm,
                         const char *line, uint32_t len)
{
  uint32_t head = shm->out_head;
  uint32_t tail = shm->out_tail;
  uint32_t used = (head >= tail) ? (head - tail)
                                 : (UAC2_ASMP_OUTBOX_SIZE - (tail - head));
  uint32_t free = UAC2_ASMP_OUTBOX_SIZE - 1u - used;

  if (len > free)
    {
      shm->out_dropped++;
      return;
    }
  /* Keep lines contiguous: wrap first if needed. */
  if (head + len > UAC2_ASMP_OUTBOX_SIZE)
    {
      head = 0;
    }
  for (uint32_t i = 0; i < len; i++)
    {
      shm->outbox[head++] = line[i];
    }
  shm->out_head = head;
}

static void commit_line(volatile struct uac2_asmp_shm_s *shm,
                        char *buf, struct emit_s *e)
{
  emit_c(e, '\n');
  outbox_write(shm, buf, (uint32_t)(e->p - buf));
}

/* ---- snapshot formatter (byte-compatible with local MON prints) ----- */

static void format_snapshot(volatile struct uac2_asmp_shm_s *shm,
                            const struct uac2_mon_snapshot_s *s)
{
  /* BSS, not stack: worker stack is tiny (see Makefile). */
  static char line[512];
  struct emit_s e;

#define BEGIN_LINE() do { e.p = line; e.end = line + sizeof(line) - 1; } while (0)

  /* [UAC2 #] */
  BEGIN_LINE();
  emit_s(&e, "[UAC2 #"); emit_u32(&e, s->tick_no); emit_s(&e, "] ");
  emit_s(&e, s->is_streaming ? "STREAMING (192kHz Active)" :
           (s->alt > 0 ? "ALT_SETTING_ACTIVE" :
            "STANDBY (Waiting Host Playback)"));
  emit_s(&e, " | Alt:"); emit_u32(&e, s->alt);
  emit_s(&e, " | SR:"); emit_u32(&e, s->sample_rate);
  emit_s(&e, " Hz | Buf:"); emit_u32(&e, s->buffered);
  emit_s(&e, " B | Under:"); emit_u32(&e, s->underrun);
  emit_s(&e, " | Over:"); emit_u32(&e, s->overrun);
  commit_line(shm, line, &e);

  /* [AUD #] */
  BEGIN_LINE();
  emit_s(&e, "[AUD #"); emit_u32(&e, s->tick_no);
  emit_s(&e, "] playing:"); emit_i32(&e, s->aplaying ? 1 : 0);
  emit_s(&e, " fd:"); emit_i32(&e, s->afd);
  emit_s(&e, " enq:"); emit_u32(&e, s->enq);
  emit_s(&e, " deq:"); emit_u32(&e, s->deq);
  emit_s(&e, " aud_udr:"); emit_u32(&e, s->aud_udr);
  emit_s(&e, " rst:"); emit_u32(&e, s->rst);
  emit_s(&e, " freetop:"); emit_i32(&e, s->freetop);
  emit_s(&e, " clk:"); emit_i32(&e, s->clk);
  emit_s(&e, " errcont:"); emit_u32(&e, s->errcont);
  commit_line(shm, line, &e);

  /* [FEED #] */
  BEGIN_LINE();
  emit_s(&e, "[FEED #"); emit_u32(&e, s->tick_no);
  emit_s(&e, "] partial:"); emit_u32(&e, s->partial);
  emit_s(&e, " empty:"); emit_u32(&e, s->empty);
  emit_s(&e, " done:"); emit_u32(&e, s->done);
  emit_s(&e, " erronly:"); emit_u32(&e, s->erronly);
  emit_s(&e, " errdone:"); emit_u32(&e, s->errdone);
  emit_s(&e, " dup:"); emit_u32(&e, s->dup);
  emit_s(&e, " gap:"); emit_u32(&e, s->gap);
  emit_s(&e, " fdup:"); emit_u32(&e, s->fdup);
  emit_s(&e, " fgap:"); emit_u32(&e, s->fgap);
  emit_s(&e, " svd:"); emit_u32(&e, s->svd);
  emit_s(&e, " svu:"); emit_u32(&e, s->svu);
  emit_s(&e, " fb:0x"); emit_hex8(&e, s->fbff);
  emit_s(&e, " ferr:"); emit_i32(&e, s->fberr);
  commit_line(shm, line, &e);

  /* [EOGAP #] */
  BEGIN_LINE();
  emit_s(&e, "[EOGAP #"); emit_u32(&e, s->tick_no); emit_s(&e, "]");
  for (int i = 0; i < 10; i++)
    {
      emit_s(&e, " g"); emit_u32(&e, (uint32_t)i);
      emit_c(&e, ':'); emit_u32(&e, s->eogap[i]);
    }
  commit_line(shm, line, &e);

  /* [DAT #] */
  BEGIN_LINE();
  emit_s(&e, "[DAT #"); emit_u32(&e, s->tick_no);
  emit_s(&e, "] dc:"); emit_u32(&e, s->dc);
  emit_s(&e, " sc:"); emit_u32(&e, s->sc);
  emit_s(&e, " raw:0x"); emit_hex8(&e, s->r_smp);
  emit_s(&e, " dst:0x"); emit_hex8(&e, s->d_smp);
  emit_s(&e, " crc:0x"); emit_hex8(&e, s->crc_val);
  emit_s(&e, " bytes:"); emit_u32(&e, s->crc_bytes);
  emit_s(&e, " isum:"); emit_u32(&e, s->iso_sum_bytes);
  emit_s(&e, " icalls:"); emit_u32(&e, s->iso_sum_calls);
  commit_line(shm, line, &e);

  /* [MON #] (+ worker heartbeat => sub-core proof on every batch) */
  BEGIN_LINE();
  emit_s(&e, "[MON #"); emit_u32(&e, s->tick_no);
  emit_s(&e, "] wakes:"); emit_u32(&e, s->pump_wakes);
  emit_s(&e, " newstream:"); emit_u32(&e, s->mon_newstream);
  emit_s(&e, "(left="); emit_u32(&e, s->mon_newstream_leftover); emit_c(&e, ')');
  emit_s(&e, " fluke:"); emit_u32(&e, s->mon_fluke_cancel);
  emit_s(&e, " revived:"); emit_u32(&e, s->mon_restart_revived);
  emit_s(&e, " failed:"); emit_u32(&e, s->mon_restart_failed);
  emit_s(&e, " enqfail:"); emit_u32(&e, s->mon_enq_fail);
  emit_s(&e, " msg_udr:"); emit_u32(&e, s->mu);
  emit_s(&e, " msg_ioe:"); emit_u32(&e, s->me);
  emit_s(&e, " subhb:"); emit_u32(&e, shm->worker_hb);
  commit_line(shm, line, &e);

  /* [REG] block */
  if (s->has_reg)
    {
      BEGIN_LINE();
      emit_s(&e, "[REG] CFG=0x"); emit_hex8(&e, s->reg_devcfg);
      emit_s(&e, " CTL=0x"); emit_hex8(&e, s->reg_devctl);
      emit_s(&e, " STS=0x"); emit_hex8(&e, s->reg_devsts);
      emit_s(&e, " INT=0x"); emit_hex8(&e, s->reg_devintr);
      emit_s(&e, " BUSY=0x"); emit_hex8(&e, s->reg_busy);
      emit_s(&e, " | EP2CTL=0x"); emit_hex8(&e, s->reg_ep2ctl);
      emit_s(&e, " EP2STS=0x"); emit_hex8(&e, s->reg_ep2sts);
      commit_line(shm, line, &e);

      BEGIN_LINE();
      emit_s(&e, "[AUDREG] ADR=0x"); emit_hex8(&e, s->au_adr);
      emit_s(&e, " SMPL=0x"); emit_hex8(&e, s->au_smpls);
      emit_s(&e, " CMD=0x"); emit_hex8(&e, s->au_cmd);
      emit_s(&e, " CHSEL=0x"); emit_hex8(&e, s->au_chsel);
      emit_s(&e, " MON=0x"); emit_hex8(&e, s->au_mon);
      emit_s(&e, " ISTAT=0x"); emit_hex8(&e, s->au_istat);
      commit_line(shm, line, &e);

      if (s->has_errsnap)
        {
          BEGIN_LINE();
          emit_s(&e, "[ERRSNAP]");
          for (int i = 0; i < 7; i++)
            {
              emit_s(&e, " "); emit_hex8(&e, s->errsnap[i]);
            }
          emit_s(&e, " errcont:"); emit_u32(&e, s->errcont);
          commit_line(shm, line, &e);
        }
      if (s->has_errsnap2)
        {
          BEGIN_LINE();
          emit_s(&e, "[ERRSNAP2]");
          for (int i = 0; i < 8; i++)
            {
              emit_s(&e, " "); emit_hex8(&e, s->errsnap2[i]);
            }
          commit_line(shm, line, &e);
        }
    }
#undef BEGIN_LINE
}

/* ---- worker entry ----------------------------------------------------- */

/* Snapshot copy (main stops touching inbox after bumping seq; the local
 * copy keeps formatting consistent and volatile-clean). */
static void snap_copy(struct uac2_mon_snapshot_s *dst,
                      volatile uint8_t *src, uint32_t n)
{
  uint8_t *d = (uint8_t *)dst;
  while (n-- > 0)
    {
      *d++ = *src++;
    }
}

int main(void)
{
  mpmutex_t mutex;
  mpshm_t shm;
  volatile struct uac2_asmp_shm_s *shared;
  /* BSS, not stack (see above). */
  static struct uac2_mon_snapshot_s snap;
  uint32_t last = 0;
  uint32_t idle = 0;

  if (mpmutex_init(&mutex, UAC2_ASMP_KEY_MUTEX) < 0)
    {
      return -1;
    }
  if (mpshm_init(&shm, UAC2_ASMP_KEY_SHM,
                 sizeof(struct uac2_asmp_shm_s)) < 0)
    {
      return -1;
    }

  shared = (volatile struct uac2_asmp_shm_s *)mpshm_attach(&shm, 0);
  if (!shared)
    {
      printf("[MONW] attach failed\n");
      return -1;
    }
  shared->dbg_stage = 1;
  printf("[MONW] attached shm=%p abi=%08lx/%lu/%lu (expect %08lx/%u/%u)\n",
         (void *)shared,
         (unsigned long)shared->abi_magic,
         (unsigned long)shared->abi_version,
         (unsigned long)shared->abi_size,
         (unsigned long)UAC2_ASMP_MAGIC, UAC2_ASMP_VERSION,
         (unsigned)sizeof(struct uac2_asmp_shm_s));

  /* ABI gate: never format a skewed layout. */
  if (shared->abi_magic != UAC2_ASMP_MAGIC ||
      shared->abi_version != UAC2_ASMP_VERSION ||
      shared->abi_size != sizeof(struct uac2_asmp_shm_s))
    {
      shared->dbg_stage = 0xBAD00000u | (shared->abi_size & 0xFFFFFu);
      mpshm_detach(&shm);
      return -1;
    }
  shared->dbg_stage = 2;

  shared->worker_ready = 1;

  for (;;)
    {
      uint32_t seq = shared->inbox_seq;
      if (seq != last)
        {
          last = seq;
          idle = 0;
          snap_copy(&snap, (volatile uint8_t *)&shared->inbox,
                    sizeof(snap));
          format_snapshot(shared, &snap);
          shared->inbox_done = seq;
        }
      else if (++idle >= 1000u)
        {
          /* Idle backoff: sub-core sleeps 100us between polls (~10kHz
           * check rate, <0.2ms format latency). A full-speed spin loop
           * wastes power and sprays bus noise next to the audio engine. */
          idle = 0;
          up_udelay(100);
        }
      shared->worker_hb++;
    }

  /* NOTREACHED */
}
