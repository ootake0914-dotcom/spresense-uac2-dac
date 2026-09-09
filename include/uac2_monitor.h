/**
 * @file uac2_monitor.h
 * @brief Non-audio telemetry offload: main/pump cores stay clean for audio.
 *
 * Design:
 *   - Audio-critical path (USB ISR -> ring -> pump thread -> /dev/pcm0 DMA)
 *     must NEVER call printf/fflush or do UART work. It only bumps
 *     volatile counters and fills cheap snapshots.
 *   - All UART formatting/printing (status lines, register dumps, EP0 setup
 *     log drain) runs in a dedicated low-priority monitor thread.
 *   - When NuttX SMP is enabled (CONFIG_SMP), the monitor thread pins
 *     itself to a sub CPU (default CPU1) via sched_setaffinity, so logging
 *     literally runs on a sub-core. The pump thread pins itself to CPU0.
 *     Without SMP the same API still isolates by priority (monitor lowest),
 *     so the build never breaks on single-core configs.
 *
 * Log format is kept byte-compatible with the previous main-loop prints
 * ([UAC2 #] / [AUD #] / [FEED #] / [EOGAP #] / [DAT #] / [REG] / [AUDREG] /
 * [ERRSNAP]) so tools/record_serial.py and test_logs parsers keep working.
 */

#ifndef __UAC2_MONITOR_H
#define __UAC2_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

/* Kconfig overrides (Spresense SDK menuconfig). Command line -D wins. */
#ifdef CONFIG_EXAMPLES_UAC2_DAC_MON_PRIORITY
#undef UAC2_MON_PRIORITY
#define UAC2_MON_PRIORITY CONFIG_EXAMPLES_UAC2_DAC_MON_PRIORITY
#endif
#ifdef CONFIG_EXAMPLES_UAC2_DAC_MON_CPU
#undef UAC2_MON_CPU
#define UAC2_MON_CPU CONFIG_EXAMPLES_UAC2_DAC_MON_CPU
#endif
#ifdef CONFIG_EXAMPLES_UAC2_DAC_PUMP_CPU
#undef UAC2_PUMP_CPU
#define UAC2_PUMP_CPU CONFIG_EXAMPLES_UAC2_DAC_PUMP_CPU
#endif

/* Monitor thread defaults (NuttX: larger number = higher priority).
 * Pump = 150, main app = 100, so monitor = 50 never preempts audio.
 */
#ifndef UAC2_MON_PRIORITY
#define UAC2_MON_PRIORITY 50
#endif

#ifndef UAC2_MON_STACKSIZE
#define UAC2_MON_STACKSIZE 4096
#endif

/* SMP affinity defaults: monitor -> sub-core, pump -> main core.
 * Honored only when CONFIG_SMP is defined; otherwise ignored.
 */
#ifndef UAC2_MON_CPU
#define UAC2_MON_CPU 1
#endif

#ifndef UAC2_PUMP_CPU
#define UAC2_PUMP_CPU 0
#endif

/* One telemetry snapshot = one former 1-second (or 3-second reg) print batch.
 * Produced by the main thread with cheap volatile/register reads only;
 * consumed + formatted by the monitor thread.
 */
struct uac2_mon_snapshot_s
{
  uint32_t tick_no;          /* 1-second tick number */

  /* [UAC2 #] fields */
  bool     is_streaming;
  uint8_t  alt;
  uint32_t sample_rate;
  uint32_t buffered;
  uint32_t underrun;
  uint32_t overrun;

  /* [AUD #] fields */
  bool     aplaying;
  int      afd;
  uint32_t enq;
  uint32_t deq;
  uint32_t aud_udr;
  uint32_t rst;
  int      freetop;
  int      clk;
  uint32_t errcont;

  /* [FEED #] fields */
  uint32_t partial;
  uint32_t empty;
  uint32_t done;
  uint32_t erronly;
  uint32_t errdone;
  uint32_t dup;
  uint32_t gap;
  uint32_t fdup;
  uint32_t fgap;
  uint32_t svd;
  uint32_t svu;
  uint32_t fbff;
  int32_t  fberr;

  /* [EOGAP #] fields */
  uint32_t eogap[10];

  /* [DAT #] fields */
  uint32_t dc;
  uint32_t sc;
  uint32_t mu;
  uint32_t me;
  uint32_t r_smp;
  uint32_t d_smp;
  uint32_t crc_val;
  uint32_t crc_bytes;
  uint32_t iso_sum_bytes;
  uint32_t iso_sum_calls;

  /* Pump-thread event counters (no UART in pump anymore) */
  uint32_t pump_wakes;
  uint32_t mon_newstream;
  uint32_t mon_newstream_leftover;
  uint32_t mon_fluke_cancel;
  uint32_t mon_restart_revived;
  uint32_t mon_restart_failed;
  uint32_t mon_enq_fail;

  /* [REG] / [AUDREG] / [ERRSNAP] raw register captures (main does loads) */
  bool     has_reg;
  uint32_t reg_devcfg;
  uint32_t reg_devctl;
  uint32_t reg_devsts;
  uint32_t reg_devintr;
  uint32_t reg_busy;
  uint32_t reg_ep2ctl;
  uint32_t reg_ep2sts;
  uint32_t au_adr;
  uint32_t au_smpls;
  uint32_t au_cmd;
  uint32_t au_chsel;
  uint32_t au_mon;
  uint32_t au_istat;
  bool     has_errsnap;
  uint32_t errsnap[8];
  bool     has_errsnap2;
  uint32_t errsnap2[8];
};

int uac2_monitor_init(void);
void uac2_monitor_push(const struct uac2_mon_snapshot_s *snap);

/* Pin calling thread to a CPU when SMP is available. No-op otherwise.
 * Shared helper so pump + monitor use identical logic.
 */
void uac2_pin_self_to_cpu(int cpu);

#endif /* __UAC2_MONITOR_H */
