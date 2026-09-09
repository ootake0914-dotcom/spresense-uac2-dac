/**
 * @file uac2_asmp.h
 * @brief ASMP sub-core offload protocol for UAC2 DAC telemetry.
 *
 * Phase 1: sub-core 1 runs a formatter worker (worker/monw). The main
 * core hands telemetry snapshots through shared memory; the worker
 * formats them into UART-ready text in an outbox ring; the main-core
 * MON thread only dumps those bytes (no printf formatting on main).
 * EP0 setup-log drain stays on main (main-core memory, tiny volume).
 *
 * Both sides share this header: ABI gate (magic/ver/size) rejects
 * skewed builds (HexaMIDI lesson: silent PCM garbage on mismatch).
 */

#ifndef __UAC2_ASMP_H
#define __UAC2_ASMP_H

#include <stdint.h>
#include "uac2_monitor.h" /* struct uac2_mon_snapshot_s (shared payload) */

#define UAC2_ASMP_MAGIC       0x55414332u /* "UAC2" */
#define UAC2_ASMP_VERSION     3u /* v3: snapshot gains iso_sum_bytes/calls */

#define UAC2_ASMP_KEY_SHM     11
#define UAC2_ASMP_KEY_MQ      12
#define UAC2_ASMP_KEY_MUTEX   13

#define UAC2_ASMP_MOUNTPT     "/romfs"
#define UAC2_ASMP_WORKER      "monw"

#define UAC2_ASMP_OUTBOX_SIZE 8192u

/* Shared memory layout (single mpshm segment, main+worker). */
struct uac2_asmp_shm_s
{
  uint32_t abi_magic;
  uint32_t abi_version;
  uint32_t abi_size;             /* == sizeof(struct uac2_asmp_shm_s) */

  /* Inbox (main -> worker, SPSC): payload then seq bump. */
  volatile uint32_t inbox_seq;   /* main increments per snapshot */
  volatile uint32_t inbox_done;  /* worker: last formatted seq */
  struct uac2_mon_snapshot_s inbox;

  /* Outbox (worker -> main, SPSC byte ring): formatted text lines. */
  volatile uint32_t out_head;    /* worker write cursor */
  volatile uint32_t out_tail;    /* main read cursor */
  volatile uint32_t out_dropped; /* worker-side drop counter */
  volatile uint32_t worker_ready;/* worker sets 1 after ABI check */
  volatile uint32_t worker_hb;   /* worker heartbeat (always advancing) */
  volatile uint32_t dbg_stage;   /* worker progress: 1=attached 2=abi-ok */
  char outbox[UAC2_ASMP_OUTBOX_SIZE];
};

/* Supervisor: boot sub-core formatter, return shared segment (or -1).
 * Stub (returns -1) when CONFIG_EXAMPLES_UAC2_DAC_ASMP is off.
 */
int uac2_asmp_boot(volatile struct uac2_asmp_shm_s **out);

#endif /* __UAC2_ASMP_H */
