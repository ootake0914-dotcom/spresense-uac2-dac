/**
 * @file uac2_audio_dma.c
 * @brief CXD5247 / S-Master Audio DMA Playback Bridge (192kHz/24bit)
 *
 * Implements high-resolution audio streaming from USB Isochronous ring buffer
 * to the Sony Spresense hardware audio subsystem (/dev/pcm0) using NuttX
 * standard audio APB (audio packet buffer) DMA queuing.
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>

#include <nuttx/audio/audio.h>
#include <arch/board/board.h>
#include <arch/chip/audio.h>
#include <arch/chip/irq.h>

/* オーディオクロック状態（arch非公開ヘッダのため手動宣言） */
extern bool cxd56_audio_clock_is_enabled(void);

#include "uac2.h"
#include "uac2_ringbuf.h"
#include "uac2_audio_dma.h"
#include "uac2_monitor.h"

#define UAC2_AUDIO_DEV_PATH         "/dev/audio/pcm0"
#define UAC2_AUDIO_DEV_ALT          "/dev/pcm0"

/* 製品形式に戻す（192kHz/24bit）。48k/16bit診断は完了したため無効化。
 * 一時診断スイッチ：1にすると正規形式48kHz/16bitで初期化する。
 */
#define UAC2_AUDIO_DIAG_FORCE_48K16 0

#define UAC2_AUDIO_NUM_BUFFERS      16
#define UAC2_AUDIO_BUFFER_SIZE      2048   /* 256 frames @ 192kHz stereo 32-bit = 1.33ms */
/* ポンプスレッドのスタック：Rev75で中間chunk[2048]は全廃したが、
 * 割込みネストに備えて8192のまま余裕を持つ（縮小しない） */
#define UAC2_PUMP_STACKSIZE         8192

/* Slot bitwidth: 24-bit PCM in 32-bit container */
#define UAC2_SLOTWIDTH_32           32u
#define UAC2_SLOTWIDTH_16           16u

/* Rev84 (5): forward declaration (pump thread calls start on newstream). */
int uac2_audio_start(void);

struct uac2_audio_dma_s
{
  int dev_fd;
  mqd_t mq;
  char mq_name[32];
  pthread_t pump_tid;
  sem_t pump_sem;
  bool is_initialized;
  volatile bool is_playing;
  volatile bool thread_run;
  /* 初期化前に開始要求が来た場合の保留フラグ（初期化順序競合の対策） */
  volatile bool start_pending;

  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bit_depth;
  uint8_t volume_percent;
  bool mute;

  /* APB Buffer Management */
  struct ap_buffer_s apbs[UAC2_AUDIO_NUM_BUFFERS] __attribute__((aligned(4)));
  uint8_t buff_mem[UAC2_AUDIO_NUM_BUFFERS][UAC2_AUDIO_BUFFER_SIZE] __attribute__((aligned(16)));
  struct ap_buffer_s *free_apbs[UAC2_AUDIO_NUM_BUFFERS];
  int free_top;

  /* Statistics */
  volatile uint32_t enqueue_count;
  volatile uint32_t dequeue_count;
  volatile uint32_t underrun_count;
  volatile uint32_t restart_needed;
  /* 一時診断：給電チャンクのデータ有無監査用 */
  volatile uint32_t data_chunks;
  volatile uint32_t silent_chunks;
  /* 一時診断・実験：部分APB（n<2048）と空読み（n==0）の計数。
   * USB供給ジッタ→ゼロ埋め混入説の検証用。
   */
  volatile uint32_t partial_chunks;
  volatile uint32_t empty_chunks;
  /* ストリーム検出用：ISO到着パケット数（write側が加算、ポンプが読む） */
  volatile uint32_t iso_pkt_count;
  /* APBシーケンス追跡（リプレイ/ドロップ検出。ペイロード非破壊）：
   * enqueue時に単調seqを採番＋log、DEQUEUE返却時に順序検査。
   * dup＝同一seqの二重完了（HWリプレイの証拠）、gap＝飛び（drop）。
   */
  uint32_t enq_seq_next;
  struct ap_buffer_s *seq_log_ptr[32];
  uint32_t seq_log_seq[32];
  uint32_t seq_log_head;
  uint32_t last_deq_seq;
  uint32_t dup_count;
  uint32_t gap_count;
  uint32_t first_dup_seq;
  uint32_t first_gap_seq;
  /* クロックドリフトサーボ用 */
  volatile uint32_t servo_tick;
  volatile uint32_t dropped_frames;
  volatile uint32_t dupped_frames;
  uint8_t histframe[8];
  /* 一時診断：ドライバ通知メッセージの到着監査用（消費されても残る） */
  volatile uint32_t msg_underrun;
  volatile uint32_t msg_ioerror;
  volatile uint32_t msg_complete;
  /* 音質確保：ポンプ内UART禁止のためイベントは計数のみ（表示はMONスレッド）。
   * NEWSTREAM/FLUKE/RESTART/ENQ失敗のprintfはここに畳み込む。 */
  volatile uint32_t pump_wakes;
  volatile uint32_t mon_newstream;
  volatile uint32_t mon_newstream_leftover;
  volatile uint32_t mon_fluke_cancel;
  volatile uint32_t mon_restart_revived;
  volatile uint32_t mon_restart_failed;
  volatile uint32_t mon_enq_fail;
  volatile int mon_enq_last_ret;
  volatile int mon_enq_last_errno;
  /* ビットパーフェクト検証用CRC32 (IEEE): リング消費バイトのみ積算。
   * NEWSTREAMでリセットし、1ストリーム=1ウィンドウ。サーボ無介入
   * (svd/svu/dup/gapゼロ) かつOver凍結ならホストファイルCRCと一致する。 */
  volatile uint32_t crc_val;
  volatile uint32_t crc_bytes;
  /* 書込側監査用: ISRが受け入れた総バイト数と回数 (二重計数切り分け用) */
  volatile uint32_t iso_sum_bytes;
  volatile uint32_t iso_sum_calls;
  /* Rev81-diag (一時): USB intakeキャプチャ (bit-perfect監査用)。
   * head=当ストリーム先頭64B, roll=直近64B(rolling),
   * frozen=前ストリーム末尾64B (quiet突入時に凍結)。 */
  volatile bool cap_need_head;
  volatile bool cap_head_valid;
  volatile bool cap_frozen_valid;
  uint8_t cap_head[64];
  uint8_t cap_roll[64];
  uint8_t cap_frozen[64];
};

static uint32_t g_crc_tab[256];
static bool g_crc_tab_ok = false;

static struct uac2_audio_dma_s g_audio_dma;
static Uac2RingBuffer g_pcm_ring;

static void uac2_crc_init(void)
{
  if (g_crc_tab_ok)
    {
      return;
    }
  for (uint32_t i = 0; i < 256u; i++)
    {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        {
          c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
      g_crc_tab[i] = c;
    }
  g_crc_tab_ok = true;
}

static inline void uac2_crc_reset(void)
{
  g_audio_dma.crc_val = 0xFFFFFFFFu;
  g_audio_dma.crc_bytes = 0;
}

static inline void uac2_crc_update(const uint8_t *p, uint32_t n)
{
  uint32_t c = g_audio_dma.crc_val;
  for (uint32_t i = 0; i < n; i++)
    {
      c = g_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
  g_audio_dma.crc_val = c;
  g_audio_dma.crc_bytes += n;
}
static volatile uint32_t g_diag_raw_sample = 0;
static volatile uint32_t g_diag_dst_sample = 0;

/* Rev76: async-feedback PI controller state (integer-only, 1ms cadence).
 * Plant: host byte rate = F * 64000 B/s (F in samples/uframe), so
 * 1 LSB of Q16.16 F ~= 0.977 B/s of correction ("1LSB ~= 1 byte/sec").
 * P gain 0.25 LSB/byte -> tau ~= 4 s (host delay is ms-scale: no hunting).
 * No D term (same reason). I absorbs crystal offset with anti-windup.
 */
#define UAC2_FB_NOMINAL_Q16  (24u << 16)   /* 192kHz: 24.0 samples/uframe */
#define UAC2_FB_TARGET_B     (64u * 1024u) /* ring target level (bytes) */
#define UAC2_FB_DEADBAND_B   1024          /* +/-1KB: zero intervention zone */
#define UAC2_FB_CLAMP_LSB    8192          /* +/-0.125/uframe (+/-0.52%).
 * Rev86g: +/-16384 tried -> host pinned +1.2% on stale saturated values,
 * ring hit wall (Over + servo chopping 24k frames). The feedback path
 * delivers sporadically (bursts then stalls), so wide authority lets a
 * stale HIGH value wreck playback. Narrow clamp keeps the loop railed
 * HIGH but harmless: fast device drains the ring, servo idles, audio
 * stays bit-perfect (proven). DO NOT widen without reliable delivery. */
#define UAC2_FB_I_MAX        (4096L * 32768L)

struct uac2_fb_pi_s
{
  int32_t lvl_filt;  /* low-passed ring level, bytes */
  int32_t istate;    /* integral accumulator, byte*ms */
};

static struct uac2_fb_pi_s g_fb_pi;
static uint32_t g_fb_last_ff = UAC2_FB_NOMINAL_Q16;
static int32_t g_fb_last_err = 0;

/* Rev84: dt-normalized PI (dt_ms = measuredCADENCE, CLOCK_MONOTONIC).
 * istate is byte*ms: istate += e * dt_ms keeps integrator gain exact
 * under pump jitter (was: assumed 1ms tick). lvl low-pass alpha scales
 * with dt as well. Callers clamp dt to [1, 50]ms.
 */
static uint32_t uac2_fb_pi_update(struct uac2_fb_pi_s *pi, uint32_t level_b,
                                  uint32_t dt_ms)
{
  /* dt-scaled low-pass (alpha ~= dt/16; absorbs +/-4KB reserve steps) */
  pi->lvl_filt += ((int32_t)level_b - pi->lvl_filt) * (int32_t)dt_ms / 16;

  int32_t e = (int32_t)UAC2_FB_TARGET_B - pi->lvl_filt;
  if (e > -UAC2_FB_DEADBAND_B && e < UAC2_FB_DEADBAND_B)
    {
      e = 0;
    }
  g_fb_last_err = e;

  int32_t p = e / 4; /* P: tau ~= 4 s */
  int32_t hi = (int32_t)UAC2_FB_NOMINAL_Q16 + UAC2_FB_CLAMP_LSB;
  int32_t lo = (int32_t)UAC2_FB_NOMINAL_Q16 - UAC2_FB_CLAMP_LSB;
  int32_t out_ol = (int32_t)UAC2_FB_NOMINAL_Q16 + p + pi->istate / 32768;

  /* Conditional integration (anti-windup): freeze the integrator only
   * when the output is already saturated AND the error pushes further
   * into the same saturation (e>0 drives up, e<0 drives down).
   */
  if (!((out_ol >= hi && e > 0) || (out_ol <= lo && e < 0)))
    {
      pi->istate += e * (int32_t)dt_ms; /* byte*ms: dt-normalized */
      if (pi->istate >  UAC2_FB_I_MAX) pi->istate =  UAC2_FB_I_MAX;
      if (pi->istate < -UAC2_FB_I_MAX) pi->istate = -UAC2_FB_I_MAX;
    }

  int32_t out = (int32_t)UAC2_FB_NOMINAL_Q16 + p + pi->istate / 32768;
  if (out > hi) out = hi;
  if (out < lo) out = lo;
  return (uint32_t)out;
}

static void free_pool_push(struct ap_buffer_s *apb)
{
  if (g_audio_dma.free_top >= UAC2_AUDIO_NUM_BUFFERS)
    {
      return;
    }
  for (int i = 0; i < g_audio_dma.free_top; i++)
    {
      if (g_audio_dma.free_apbs[i] == apb)
        {
          return; /* Avoid duplicate */
        }
    }
  g_audio_dma.free_apbs[g_audio_dma.free_top++] = apb;
}

static struct ap_buffer_s *free_pool_pop(void)
{
  if (g_audio_dma.free_top <= 0)
    {
      return NULL;
    }
  return g_audio_dma.free_apbs[--g_audio_dma.free_top];
}

/* APBシーケンス追跡ヘルパー（リプレイ/ドロップ検出用） */
static void track_enqueue(struct ap_buffer_s *apb)
{
  uint32_t s = ++g_audio_dma.enq_seq_next;
  g_audio_dma.seq_log_ptr[g_audio_dma.seq_log_head] = apb;
  g_audio_dma.seq_log_seq[g_audio_dma.seq_log_head] = s;
  g_audio_dma.seq_log_head = (g_audio_dma.seq_log_head + 1) & 31u;
}

static void track_dequeue(struct ap_buffer_s *apb)
{
  uint32_t s = 0;
  for (int i = 0; i < 32; i++)
    {
      if (g_audio_dma.seq_log_ptr[i] == apb)
        {
          s = g_audio_dma.seq_log_seq[i];
          g_audio_dma.seq_log_ptr[i] = NULL; /* consume: stale match厳禁 */
          break;
        }
    }
  if (s == 0)
    {
      return; /* unknown (pre-track / flushed) */
    }
  if (s == g_audio_dma.last_deq_seq + 1)
    {
      g_audio_dma.last_deq_seq = s;
      return;
    }
  if (s <= g_audio_dma.last_deq_seq)
    {
      if (g_audio_dma.first_dup_seq == 0)
        {
          g_audio_dma.first_dup_seq = s;
        }
      g_audio_dma.dup_count++;
    }
  else
    {
      if (g_audio_dma.first_gap_seq == 0)
        {
          g_audio_dma.first_gap_seq = s;
        }
      g_audio_dma.gap_count++;
      g_audio_dma.last_deq_seq = s;
    }
}

static void drain_audio_msgs(mqd_t mq)
{
  struct audio_msg_s msg;
  for (;;)
    {
      ssize_t size = mq_receive(mq, (char *)&msg, sizeof(msg), NULL);
      if (size != sizeof(msg))
        {
          break; /* EAGAIN: empty queue */
        }
      switch (msg.msg_id)
        {
          case AUDIO_MSG_DEQUEUE:
            g_audio_dma.dequeue_count++;
            if (msg.u.ptr)
              {
                track_dequeue((struct ap_buffer_s *)msg.u.ptr);
                free_pool_push((struct ap_buffer_s *)msg.u.ptr);
              }
            /* DEQUEUE起床は廃止（timedwait化により不要化。
             * 起床postが飢餓・スピンの温床になるため）。
             */
            break;
          case AUDIO_MSG_UNDERRUN:
            g_audio_dma.underrun_count++;
            g_audio_dma.msg_underrun++;
            /* 音質確保：オーディオ経路でUART禁止。計数のみ（MON表示）。 */
            g_audio_dma.restart_needed = 1;
            break;
          case AUDIO_MSG_IOERROR:
            g_audio_dma.msg_ioerror++;
            /* 音質確保：オーディオ経路でUART禁止。計数のみ（MON表示）。 */
            g_audio_dma.restart_needed = 2;
            break;
          case AUDIO_MSG_COMPLETE:
            /* STOPPING完了（flush＋CONFIGURED到達）の証拠。再始動の
             * 前提条件として使用する（drainで回収も兼ねる）。
             */
            g_audio_dma.msg_complete++;
            break;
          default:
            break;
        }
    }
}

static void *uac2_audio_pump_thread(void *arg)
{
  (void)arg;
  /* Rev75: 中間バッファ chunk[2048] を全廃しリング→APB直読みシングルコピー化。
   * CPUバススイッチングノイズ・高周波GNDバウンス極小化（ピュアオーディオ対策）。
   */

  printf("[UAC2-AUDIO] Pump thread started (priority 150)\n");

  /* 音質確保：ポンプはメインCPUに固定し、MON（ログ）はサブコアへ。
   * シングルコアビルドでは何もしない。 */
  uac2_pin_self_to_cpu(UAC2_PUMP_CPU);

  /* 一時診断：ポンプ自身の定期報告は廃止（UARTはMONスレッド専用）。
   * wakesはカウンタに残し、1秒スナップショット経由でMONが表示する。 */
  /* ストリーム検出（ESP new_play儀式）：50ms以上の無到着gap後の初到着を
   * 新ストリームとし、前ストリーム残渣を捨てて位相を確定させる。
   * tail=headの1ストアのみ（memset掃除は競合・遅延のため不可）。
   */
  uint32_t last_pc = 0;
  uint32_t quiet_wakes = 0;
  uint32_t stream_seq = 0;
  uint32_t last_frozen_pc = 0; /* Rev81-diag: frozen済みpc (二重凍結防止) */

  while (g_audio_dma.thread_run)
    {
      /* timedwait起床（最大2ms周期：供給即応でpartial削減。
       * USB起床（ISO到着即時）と併用。5msではジッタ吸収が不足した）。
       */
      {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000L; /* +2ms */
        if (ts.tv_nsec >= 1000000000L)
          {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
          }
        sem_timedwait(&g_audio_dma.pump_sem, &ts);
      }
      g_audio_dma.pump_wakes++;

      /* 音質確保：ポンプ内のUART定期報告は全廃（MONスレッドが1秒毎に表示）。
       * ここでは計数のみ。TX割込み・バス競合をオーディオ経路から排除する。 */

      if (!g_audio_dma.is_playing || g_audio_dma.dev_fd < 0)
        {
          /* Idle: reset the feedback loop and hold nominal so no
           * windup accumulates while the ring sits empty.
           */
          g_fb_pi.lvl_filt = 0;
          g_fb_pi.istate = 0;
          g_fb_last_err = 0;
          g_fb_last_ff = UAC2_FB_NOMINAL_Q16;
          uac2_feedback_update(UAC2_FB_NOMINAL_Q16);
          continue;
        }

      /* Rev76: async-feedback PI @1ms cadence (task context).
       * Refreshes the EP1 IN payload the host polls every 1ms.
       */
      /* Rev84: async-feedback PI on CLOCK_MONOTONIC with measured dt.
       * REALTIME is wrong for control loops (NTP steps); the fixed-1ms
       * assumption mis-scales gains under pump jitter. dt is clamped to
       * [1,50]ms so revive stalls cannot wind up the integrator.
       */
      {
        static struct timespec fb_last = {0, 0};
        static bool fb_last_valid = false;
        struct timespec fb_now;
        long fb_dms;
        uint32_t dt_ms;
        clock_gettime(CLOCK_MONOTONIC, &fb_now);
        if (!fb_last_valid)
          {
            fb_last = fb_now;
            fb_last_valid = true;
          }
        fb_dms = (fb_now.tv_sec - fb_last.tv_sec) * 1000L +
                 (fb_now.tv_nsec - fb_last.tv_nsec) / 1000000L;
        if (fb_dms >= 1)
          {
            dt_ms = (uint32_t)fb_dms;
            if (dt_ms > 50u)
              {
                dt_ms = 50u;
              }
            fb_last = fb_now;
#if UAC2_FB_NOMINAL_LOCK
            /* Rev87-E1: hold exact nominal (drift probe, no regulation). */
            g_fb_last_ff = UAC2_FB_LOCK_VALUE;
#else
            g_fb_last_ff = uac2_fb_pi_update(
                               &g_fb_pi,
                               uac2_ringbuf_available_read(&g_pcm_ring),
                               dt_ms);
#endif
            uac2_feedback_update(g_fb_last_ff);
            uac2_feedback_poll();
          }
      }

      /* ストリーム開始検出：50ms gap（25wake）後の初到着で残渣snap */
      {
        uint32_t pc = g_audio_dma.iso_pkt_count;
        if (pc != last_pc)
          {
            if (quiet_wakes >= 25)
              {
                uint32_t left = uac2_ringbuf_available_read(&g_pcm_ring);
                if (left > 0)
                  {
                    uac2_ringbuf_flush(&g_pcm_ring);
                  }
                stream_seq++;
                /* 音質確保：UART禁止。MONスレッドが表示する。 */
                g_audio_dma.mon_newstream = stream_seq;
                g_audio_dma.mon_newstream_leftover = left;
                /* 新ストリーム=新CRCウィンドウ */
                uac2_crc_reset();
                /* Rev81-diag: 次writeが新頭を採取 (frozenはquiet突入時に確定済み) */
                g_audio_dma.cap_need_head = true;
                /* Rev84 (5): explicit stream-start transition. Ensures the
                 * engine runs (covers config-0 stop -> stream without a
                 * fresh SET_CONFIG). No-op when already playing (no ioctl).
                 * NOTE: must stay a no-op in the common case — issuing a
                 * real START here while a STOP is still STOPPING hangs the
                 * pump (SDK corner). See the removed auto-stop above.
                 * First packets are already safe in the 128KB ring.
                 */
                uac2_audio_start();
              }
            last_pc = pc;
            quiet_wakes = 0;
          }
        else if (quiet_wakes < 1000000u)
          {
            quiet_wakes++;
            /* Rev81-diag: ストリーム終端確定でrolling=旧末尾を凍結。
             * 新頭armは newstream側 (初回到着時) で行う。 */
            if (quiet_wakes == 25 && last_pc != last_frozen_pc)
              {
                memcpy(g_audio_dma.cap_frozen, g_audio_dma.cap_roll, 64u);
                g_audio_dma.cap_frozen_valid = true;
                last_frozen_pc = last_pc;
              }
            /* Rev84 (5): auto-stop REMOVED (was: stop engine at quiet>=250).
             * Reason: AUDIOIOC_START issued from newstream while a prior
             * STOP is still STOPPING hangs the pump forever (SDK corner:
             * STOPPING-complete ISR never arrives -> START never returns).
             * Observed: pump death, intake overruns, frozen CRC. The engine
             * stays running across streams (zero-fill idle); restart on
             * newstream is a safe no-op via is_playing. A future fix needs
             * msg_complete-gated START (async), not stop/start pairing.
             */
          }
      }

      drain_audio_msgs(g_audio_dma.mq);

      /* Handle underrun recovery v5 (fluke-gate):
       * Rev61教訓：play開始時の一過性dipによるUNDERUN Takes健康的
       * エンジンをrestart(STOP/refill)が殺す。ゲート：50ms×2窓で
       * deqが両方>=10（約200/s以上で定常動作）ならFLUKEと判定し
       * 何もせず給電継続。両窓いずれか不振なら本物としてv4手順へ。
       * （flushバースト(+14単発)と定常(+37/+37)を分離できる）
       */
      if (g_audio_dma.restart_needed)
        {
          g_audio_dma.restart_needed = 0;
          if (!g_audio_dma.is_playing)
            {
              /* Rev84 (5): stale recovery request for an intentionally
               * stopped engine (stream-end auto-stop). The underrun
               * message predates the stop; drain the queue and ignore
               * instead of pointlessly restarting an idle engine.
               */
              drain_audio_msgs(g_audio_dma.mq);
            }
          else
            {
          /* Rev84 (5) fast-path REMOVED (was: skip fluke-gate when
           * quiet>=250). Reason: the gate's 100ms stall is HARMLESS
           * post-close (quiet = no intake, nothing to overrun), while
           * skipping it leaves a genuinely dead engine unrestored
           * (observed: pump alive, CRC frozen, intake overruns, no
           * revived/failed movement). Fluke-gate always runs; a dead
           * engine gets its revive.
           */
          uint32_t g0 = g_audio_dma.dequeue_count;
          usleep(50000);
          drain_audio_msgs(g_audio_dma.mq);
          uint32_t g1 = g_audio_dma.dequeue_count;
          usleep(50000);
          drain_audio_msgs(g_audio_dma.mq);
          uint32_t g2 = g_audio_dma.dequeue_count;
          if ((g1 - g0) >= 10 && (g2 - g1) >= 10)
            {
              /* 音質確保：UART禁止。MONスレッドが表示する。 */
              g_audio_dma.mon_fluke_cancel++;
            }
          else
            {
          uint32_t deq_entry = g_audio_dma.dequeue_count;
          uint32_t comp_entry = g_audio_dma.msg_complete;
          int stopped_once = 0;
          int revived = 0;
          int consec = 0;
          int start_errs = 0; /* Rev84 (4): consecutive START failures */
          for (int t = 0; t < 300; t++)
            {
              drain_audio_msgs(g_audio_dma.mq);
              /* Rev85b: post-silence sleep/wake. quiet>=250 (no traffic)
               * + underrun means the CXD5247 slept on sustained digital
               * silence (metronomic ~5s episodes, ENQUEUE rejected while
               * asleep, music streams immune). A sleeping engine needs
               * START-only: AUDIOIOC_STOP here risks the SDK corner
               * (STOPPING-complete ISR never comes -> STOP hangs the
               * pump -> intake overruns + hard lockup + watchdog reboot,
               * observed once). Mid-stream death (quiet small) keeps the
               * full STOP for a genuine reset.
               */
              if (!stopped_once && quiet_wakes < 250 &&
                  (g_audio_dma.msg_complete > comp_entry ||
                   g_audio_dma.dequeue_count > deq_entry))
                {
                  int stop_ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_STOP, 0);
                  stopped_once = 1;
                  (void)stop_ret;
                  drain_audio_msgs(g_audio_dma.mq);
                }
              int refill = 0;
              while (g_audio_dma.free_top > 0)
                {
                  struct ap_buffer_s *apb = free_pool_pop();
                  if (!apb) break;
                  memset(apb->samp, 0, apb->nmaxbytes);
                  apb->nbytes = apb->nmaxbytes;
                  apb->curbyte = 0;
                  struct audio_buf_desc_s bd;
                  bd.numbytes = apb->nmaxbytes;
                  bd.u.buffer = apb;
                  int eret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&bd);
                  if (eret == 0)
                    {
                      g_audio_dma.enqueue_count++;
                      track_enqueue(apb);
                      refill++;
                    }
                  else
                    {
                      free_pool_push(apb);
                      break;
                    }
                }
              int start_ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_START, 0);
              if (start_ret < 0)
                {
                  start_errs++;
                }
              else
                {
                  start_errs = 0;
                }
              (void)refill;
              uint32_t deq0 = g_audio_dma.dequeue_count;
              usleep(3000);
              drain_audio_msgs(g_audio_dma.mq);
              uint32_t deq1 = g_audio_dma.dequeue_count;
              /* 音質確保：try毎のUART禁止（300回分のprintfはジッタ源）。 */
              if ((deq1 - deq0) >= 2)
                {
                  consec++;
                  if (consec >= 2)
                    {
                      revived = 1;
                      break;
                    }
                }
              else
                {
                  consec = 0;
                }
            }
          /* 音質確保：結果は計数のみ。MONスレッドが表示する。 */
          if (revived)
            {
              g_audio_dma.mon_restart_revived++;
            }
          else
            {
              g_audio_dma.mon_restart_failed++;
              /* Rev84 (4): persistent START failure is fatal-class.
               * deq-based revive failing is one thing; the engine
               * refusing START 300 times straight means hardware-level
               * death. Say so loudly (once) instead of spinning forever.
               */
              if (start_errs >= 300)
                {
                  static bool s_start_fatal_logged = false;
                  if (!s_start_fatal_logged)
                    {
                      s_start_fatal_logged = true;
                      printf("[UAC2-AUDIO] FATAL: AUDIOIOC_START failed 300x "
                             "straight (engine dead, check CXD5247/clock)\n");
                      fflush(stdout);
                    }
                }
            }
            } /* end else (genuine flat death) */
            } /* end else (engine playing; Rev84 (5) stale-guard) */
        }

      /* リングバッファ→空きAPBへ給電する。最優先原則：エンジンを飢餓に
       * しない（underrunはSDKの回復不能コーナー：STOPPING完了ISRが来ず
       * STOPは永久hangする）。データ不足分は無音で埋める。
       * （B1部分待ちは終端残渣で固着→underrun→wedgeを起こすため廃止）
       */
      while (g_audio_dma.is_playing)
        {
          drain_audio_msgs(g_audio_dma.mq);

          /* 常時給電（H-α cap撤廃）：空きがある限り投入し cushon を
           * 最大16に保つ。飢餓（SDKの回復不能コーナー）を原理的に防ぐ。
           */
          if (g_audio_dma.free_top <= 0)
            {
              break; /* All DMA buffers in flight */
            }

          struct ap_buffer_s *apb = free_pool_pop();
          if (!apb)
            {
              break;
            }

          uint32_t avail = uac2_ringbuf_available_read(&g_pcm_ring);
          /* Rev73: Multi-tier Proportional Clock Drift Servo.
           * Total capacity is 128KB (~85ms). Target level is ~64KB (~42ms).
           * Deadband: 32KB - 64KB (zero intervention, pure passthrough).
           * High levels trigger proportional frame dropping (8 bytes/frame)
           * to strictly prevent buffer from ever reaching 128KB wall (Overrun).
           * Rev84: compiled out when UAC2_SERVO_ENABLE=0 (audit mode).
           */
#if UAC2_SERVO_ENABLE
          if (avail > 64u * 1024u)
            {
              g_audio_dma.servo_tick++;
              int drop_frames = 0;
              if (avail > 112u * 1024u)
                {
                  /* Critical zone: drop 2 frames every wake (~128KB/s drain) */
                  drop_frames = 2;
                }
              else if (avail > 96u * 1024u)
                {
                  /* High danger: drop 1 frame every wake (~64KB/s drain) */
                  drop_frames = 1;
                }
              else if (avail > 80u * 1024u)
                {
                  /* Alert zone: drop 1 frame every 2 wakes (~32KB/s drain) */
                  if ((g_audio_dma.servo_tick & 1u) == 0) drop_frames = 1;
                }
              else
                {
                  /* Slight drift: drop 1 frame every 8 wakes (~8KB/s drain) */
                  if ((g_audio_dma.servo_tick & 7u) == 0) drop_frames = 1;
                }

              if (drop_frames > 0)
                {
                  uint8_t drop_buf[16];
                  uint32_t bytes_to_drop = (uint32_t)drop_frames * 8u;
                  uac2_ringbuf_read(&g_pcm_ring, drop_buf, bytes_to_drop);
                  g_audio_dma.dropped_frames += drop_frames;
                  avail = uac2_ringbuf_available_read(&g_pcm_ring);
                }
            }
#endif /* UAC2_SERVO_ENABLE (drop servo) */
          /* リングreserve・二層式：2APB分（4096B≒2.7ms）未満の端数は、
           * in-flightが十分（>=8、cushion約10ms以上）ある時だけ待つ。
           * 供給ジッタのdipによるゼロ埋めpartial（可聴プツプツ）を消す。
           * in-flightが薄い時（起動ランプ・終端・回復中）は待たずに
           * 放出する（partial許容）：飢餓死より1chopがまし。
           * avail==0（真アイドル）はフル無音を流す（飢餓防止は維持）。
           * 終端残渣での固着（B1の轍）を避けるため、50ms以上無到着
           * （quiet：終端と判定）なら待たずに放出する。
           */
          int inflight = UAC2_AUDIO_NUM_BUFFERS - g_audio_dma.free_top;
          if (avail > 0 && avail < 2 * UAC2_AUDIO_BUFFER_SIZE &&
              quiet_wakes < 25 && inflight >= 8)
            {
              free_pool_push(apb);
              break; /* wait for more (reserve, safe: fat cushion) */
            }
          uint32_t n = (avail >= UAC2_AUDIO_BUFFER_SIZE) ?
                       UAC2_AUDIO_BUFFER_SIZE : avail;
          /* dup-trim：浅い水位からのフルtake時に直前frameを1つ反復し、
           * 消費を8B遅らせて水位低下を相殺する（8takeに1回まで）。
           * Rev84: UAC2_SERVO_ENABLE=0では無効（pure passthrough）。
           */
          int dup_this = 0;
#if UAC2_SERVO_ENABLE
          if (n == UAC2_AUDIO_BUFFER_SIZE && avail < 4096u)
            {
              g_audio_dma.servo_tick++;
              if ((g_audio_dma.servo_tick & 7u) == 0)
                {
                  dup_this = 1;
                }
            }
#endif /* UAC2_SERVO_ENABLE (dup-trim) */
          if (n == 0)
            {
              g_audio_dma.empty_chunks++;
            }
          else if (n < UAC2_AUDIO_BUFFER_SIZE)
            {
              g_audio_dma.partial_chunks++;
            }
          /* Rev75: シングルコピー — リングからAPB (apb->samp) へ直読み。
           * 中間 chunk[2048] 経由のダブルコピーを廃し、CPUバス転送を半減。
           */
          if (dup_this)
            {
              /* 直前frame反復を先頭に挿入し、新規消費を2040Bに抑える。
               * APBは2048Bのまま（ペイロード破壊なし・サイズ不変）。
               */
              memcpy(apb->samp, g_audio_dma.histframe, 8);
              uac2_ringbuf_read(&g_pcm_ring,
                                (uint8_t *)apb->samp + 8,
                                UAC2_AUDIO_BUFFER_SIZE - 8);
              g_audio_dma.dupped_frames++;
              /* CRCは新規消費分のみ（反復8Bは除外。dup発火時は
               * 照合対象外になることをsvd/svuで確認する） */
              uac2_crc_update((const uint8_t *)apb->samp + 8,
                              UAC2_AUDIO_BUFFER_SIZE - 8);
            }
          else
            {
              if (n > 0)
                {
                  uac2_ringbuf_read(&g_pcm_ring, (uint8_t *)apb->samp, n);
                  /* ゼロパディング部は除外し、実ストリームバイトのみ */
                  uac2_crc_update((const uint8_t *)apb->samp, n);
                }
              if (n < UAC2_AUDIO_BUFFER_SIZE)
                {
                  memset((uint8_t *)apb->samp + n, 0,
                         UAC2_AUDIO_BUFFER_SIZE - n);
                }
            }
          /* Rev74: True Direct 24-bit MSB-Aligned Passthrough.
           * USB Audio 2.0 specification (Type I Formats) defines that 24-bit PCM
           * in 4-byte subslots is inherently MSB-aligned (left-justified: bits 8-31
           * contain audio data, bits 0-7 are zero-padding).
           * CXD5602/CXD5247 hardware DAC in 32-bit slot mode expects this exact
           * MSB-aligned format.
           * Therefore, host data is passed directly without software scaling or DSP,
           * eliminating false 8-bit left shifts (+48dB excessive gain & wrap-around distortion).
           * (Rev75: chunk経由memcpyは廃止。上記直読みがそのまま該当する)
           */
          {
            const uint32_t *src32 = (const uint32_t *)apb->samp;
            if (src32[0] != 0u)
              {
                g_diag_raw_sample = src32[0];
                g_diag_dst_sample = src32[0];
              }
          }
          /* ヒストリ更新（dup用直前フレーム保持） */
          memcpy(g_audio_dma.histframe,
                 (const uint8_t *)apb->samp + UAC2_AUDIO_BUFFER_SIZE - 8, 8);

          /* Rev75: 毎バッファ舐めていた非ゼロ監査ループ
           * (data_chunks/silent_chunks) はバス負荷となるためバイパス。
           * カウンタ/APIは互換維持のため残すが、ここでは加算しない。
           */
          apb->nbytes = UAC2_AUDIO_BUFFER_SIZE;
          apb->curbyte = 0;

          struct audio_buf_desc_s bd;
          bd.numbytes = UAC2_AUDIO_BUFFER_SIZE;
          bd.u.buffer = apb;

          int ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&bd);
          if (ret == 0)
            {
              g_audio_dma.enqueue_count++;
              track_enqueue(apb);
            }
          else
            {
              /* 音質確保：投入失敗もUART禁止。計数＋最終errnoのみ保持し、
               * MONスレッドが1秒毎に表示する。 */
              g_audio_dma.mon_enq_fail++;
              g_audio_dma.mon_enq_last_ret = ret;
              g_audio_dma.mon_enq_last_errno = errno;
              free_pool_push(apb);
              break;
            }
        }
    }

  printf("[UAC2-AUDIO] Pump thread exiting\n");
  return NULL;
}

int uac2_audio_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels)
{
  memset(&g_audio_dma, 0, sizeof(g_audio_dma));
  g_audio_dma.dev_fd = -1;
  g_audio_dma.sample_rate = sample_rate;
  g_audio_dma.channels = channels;
  g_audio_dma.bit_depth = bit_depth;
  g_audio_dma.volume_percent = UAC2_AUDIO_DEFAULT_VOLUME;
  g_audio_dma.mute = false;

  uac2_ringbuf_init(&g_pcm_ring);
  uac2_crc_init();
  uac2_crc_reset();
  /* Rev81-diag: 初ストリームの頭を採取するためarm (memsetで全偽済み) */
  g_audio_dma.cap_need_head = true;

  /* SMP hardening: USB-ISR (uac2_audio_write) can fire as soon as the host
   * enumerates, i.e. long before init finishes (SET_CONFIG races power-up).
   * sem_post on an uninitialized sem is fatal on SMP (garbage spinlock),
   * benign-looking on UP. Init the pump sem FIRST so ISR contact is safe.
   */
  sem_init(&g_audio_dma.pump_sem, 0, 0);

  /* Rev68: True 192kHz Native Playback Fix.
   * CXD5247 S-Master cannot be switched on-the-fly (hot-switched).
   * Perform full power cycle: poweroff -> set_clkmode(HIRES) -> poweron.
   */
  printf("[UAC2-AUDIO] Ensuring Audio Subsystem is powered down before clock setup...\n");
  board_audio_power_control(false);

  printf("[UAC2-AUDIO] Setting Audio Clock Mode to HIRES (192kHz)...\n");
  CXD56_AUDIO_ECODE clk_err = cxd56_audio_set_clkmode(CXD56_AUDIO_CLKMODE_HIRES);
  if (clk_err != CXD56_AUDIO_ECODE_OK)
    {
      printf("[UAC2-AUDIO] WARNING: cxd56_audio_set_clkmode failed: %d\n", clk_err);
    }
  else
    {
      printf("[UAC2-AUDIO] Audio Clock Mode successfully set to HIRES!\n");
    }

  printf("[UAC2-AUDIO] Powering on CXD5247 codec with 192kHz HIRES clock...\n");
  board_audio_power_control(true);

  printf("[UAC2-AUDIO] Disabling MIC input circuits (clean DAC mode)...\n");
  cxd56_audio_dis_input();

  printf("[UAC2-AUDIO] Unmuting external headphone amplifier...\n");
  board_external_amp_mute_control(false);

  /* TRM Table 3.15-19: err_I2SxO = sample loss from command-issuance or
   * transfer delays. Audio must preempt USB so inject is never late.
   * BUT: NuttX spin_lock_irqsave is BASEPRI-masked (USEBASEPRI) and
   * IGNORES the lock on UP builds -- an audio ISR above the mask (0x60)
   * preempts "locked" dq_put/dq_get regions reentrantly and corrupts the
   * DMA queues (Rev53-56 deaths). So: keep AUDIO at OS-default level
   * (0x80, stays under the BASEPRI mask like every other driver) and
   * DEMOTE USB below it (0xA0). Same relative order, OS-safe levels.
   * USB ISR latency +~10us is harmless (125us microframes, FIFOs).
   */
  {
    const uint32_t ipr_base = 0xE000E400u;
    /* restore audio to default */
    const int aud[2] = { (CXD56_IRQ_AUDIO_1 - 16), (CXD56_IRQ_AUDIO_2 - 16) };
    /* demote USB below audio */
    const int usb[2] = { (CXD56_IRQ_USB_INT - 16), (CXD56_IRQ_USB_SYS - 16) };
    for (int k = 0; k < 2; k++)
      {
        volatile uint32_t *ipr = (volatile uint32_t *)(ipr_base + (uint32_t)(aud[k] >> 2) * 4u);
        uint32_t shift = (uint32_t)(aud[k] & 3) * 8u;
        uint32_t v = *ipr;
        v = (v & ~(0xffu << shift)) | (0x80u << shift);
        *ipr = v;
        uint32_t rb = ((*ipr) >> shift) & 0xffu;
        printf("[UAC2-AUDIO] NVIC IRQ %d prio restored 0x80, readback 0x%02lx\n",
               aud[k], (unsigned long)rb);
      }
    for (int k = 0; k < 2; k++)
      {
        volatile uint32_t *ipr = (volatile uint32_t *)(ipr_base + (uint32_t)(usb[k] >> 2) * 4u);
        uint32_t shift = (uint32_t)(usb[k] & 3) * 8u;
        uint32_t v = *ipr;
        v = (v & ~(0xffu << shift)) | (0xa0u << shift);
        *ipr = v;
        uint32_t rb = ((*ipr) >> shift) & 0xffu;
        printf("[UAC2-AUDIO] NVIC IRQ %d (USB) demoted 0xa0, readback 0x%02lx\n",
               usb[k], (unsigned long)rb);
      }
  }

  /* Open audio device */
  g_audio_dma.dev_fd = open(UAC2_AUDIO_DEV_PATH, O_RDWR | O_CLOEXEC);
  if (g_audio_dma.dev_fd < 0)
    {
      g_audio_dma.dev_fd = open(UAC2_AUDIO_DEV_ALT, O_RDWR | O_CLOEXEC);
      printf("[UAC2-AUDIO] Opened alt path %s fd=%d\n",
             UAC2_AUDIO_DEV_ALT, g_audio_dma.dev_fd);
    }
  else
    {
      printf("[UAC2-AUDIO] Opened %s fd=%d\n",
             UAC2_AUDIO_DEV_PATH, g_audio_dma.dev_fd);
    }
  fflush(stdout);

  if (g_audio_dma.dev_fd < 0)
    {
      printf("[UAC2-AUDIO] ERROR: Failed to open %s (errno=%d)\n",
             UAC2_AUDIO_DEV_PATH, errno);
      return -ENODEV;
    }

  /* Create non-blocking POSIX message queue for DMA callbacks */
  snprintf(g_audio_dma.mq_name, sizeof(g_audio_dma.mq_name), "/mq_uac2_%d", (int)getpid());
  struct mq_attr attr;
  attr.mq_maxmsg  = 128; /* flush flood時のDEQUEUE取りこぼし防止 */
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = O_NONBLOCK;

  g_audio_dma.mq = mq_open(g_audio_dma.mq_name, O_RDWR | O_CREAT | O_NONBLOCK, 0644, &attr);
  if (g_audio_dma.mq == (mqd_t)-1)
    {
      printf("[UAC2-AUDIO] ERROR: mq_open failed (errno=%d)\n", errno);
      close(g_audio_dma.dev_fd);
      g_audio_dma.dev_fd = -1;
      return -EIO;
    }

  if (ioctl(g_audio_dma.dev_fd, AUDIOIOC_REGISTERMQ, (unsigned long)g_audio_dma.mq) < 0)
    {
      printf("[UAC2-AUDIO] ERROR: AUDIOIOC_REGISTERMQ failed (errno=%d)\n", errno);
      mq_close(g_audio_dma.mq);
      close(g_audio_dma.dev_fd);
      g_audio_dma.dev_fd = -1;
      return -EIO;
    }

  /* Configure audio format: 192kHz, 2ch, 32-bit container */
  struct audio_caps_desc_s cap_desc;
  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels = channels;
  cap_desc.caps.ac_controls.hw[0] = (uint16_t)(sample_rate & 0xffff);
#if UAC2_AUDIO_DIAG_FORCE_48K16
  cap_desc.caps.ac_controls.b[2] = 16; /* 診断時：正規16bit */
#else
  cap_desc.caps.ac_controls.b[2] = UAC2_SLOTWIDTH_32; /* 32-bit slot */
#endif
  cap_desc.caps.ac_controls.b[3] = (uint8_t)((sample_rate >> 16) & 0xff);

  int ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap_desc);
  if (ret < 0)
    {
      printf("[UAC2-AUDIO] WARNING: AUDIOIOC_CONFIGURE returned %d\n", ret);
    }

  /* Set initial volume (100% / 0dB) - digital headroom is handled in pump thread */
  struct audio_caps_desc_s vol_desc;
  memset(&vol_desc, 0, sizeof(vol_desc));
  vol_desc.caps.ac_len = sizeof(struct audio_caps_s);
  vol_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  vol_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
  vol_desc.caps.ac_controls.hw[0] = 1000; /* 0dB (max) */
  ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&vol_desc);

  /* NOTE: In Rev66, on-the-fly live switching to 192k caused mute.
   * In Rev68, this is solved cleanly by configuring CXD56_AUDIO_CLKMODE_HIRES
   * during initial power cycle before board_audio_power_control(true).
   */

  /* Initialize APB pool */
  g_audio_dma.free_top = 0;
  for (int i = 0; i < UAC2_AUDIO_NUM_BUFFERS; i++)
    {
      g_audio_dma.apbs[i].nmaxbytes = UAC2_AUDIO_BUFFER_SIZE;
      g_audio_dma.apbs[i].nbytes = UAC2_AUDIO_BUFFER_SIZE;
      g_audio_dma.apbs[i].curbyte = 0;
      g_audio_dma.apbs[i].flags = 0;
      g_audio_dma.apbs[i].samp = &g_audio_dma.buff_mem[i][0];
      memset(g_audio_dma.apbs[i].samp, 0, UAC2_AUDIO_BUFFER_SIZE);
      nxmutex_init(&g_audio_dma.apbs[i].lock);

      /* 全16個を初期投入（H-α capは撤廃。TRM定義によりERRは
       * フェッチ遅延であり位相固着ではない。深い cushion が正義）。
       */

      /* Pre-enqueue silence buffers into DMA queue */
      struct audio_buf_desc_s buf_desc;
      buf_desc.numbytes = UAC2_AUDIO_BUFFER_SIZE;
      buf_desc.u.buffer = &g_audio_dma.apbs[i];
      int enq_ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&buf_desc);
      if (enq_ret < 0)
        {
          free_pool_push(&g_audio_dma.apbs[i]);
        }
      else
        {
          g_audio_dma.enqueue_count++;
          track_enqueue(&g_audio_dma.apbs[i]);
        }
    }

  /* pump_sem was already inited at the top of init (SMP hardening:
   * ISR contact before this point must be safe). Do NOT re-init here:
   * posts may already be queued from early ISO traffic. */
  g_audio_dma.thread_run = true;

  /* Spawn pump thread with high priority */
  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  pthread_attr_setstacksize(&pattr, UAC2_PUMP_STACKSIZE);
  struct sched_param sparam;
  sparam.sched_priority = 150;
  pthread_attr_setschedparam(&pattr, &sparam);

  ret = pthread_create(&g_audio_dma.pump_tid, &pattr, uac2_audio_pump_thread, NULL);
  pthread_attr_destroy(&pattr);
  if (ret != 0)
    {
      /* P0: abort init. A half-built pipeline (is_initialized=true with no
       * pump) would accept USB traffic into a ring nobody drains: silent
       * overrun storm + wedged engine. Fail loudly instead.
       */
      printf("[UAC2-AUDIO] ERROR: Failed to create pump thread: %d (aborting init)\n", ret);
      g_audio_dma.thread_run = false;
      return -ret;
    }

  g_audio_dma.is_initialized = true;

  /* 初期化完了前にSET_CONFIG等で開始要求されていた場合はここで開始する。
   * （USB列挙が音声初期化より先に完了する順序競合への対策。無音バッファが
   * 事前投入済みのため、開始しても無音再生になるだけで安全）
   */
  if (g_audio_dma.start_pending)
    {
      g_audio_dma.start_pending = false;
      printf("[UAC2-AUDIO] Reserved start request detected, starting now\n");
      uac2_audio_start();
    }

  printf("[UAC2-AUDIO] CXD5247 S-Master Ready: %lu Hz / %u ch / 32-bit slot\n",
         (unsigned long)sample_rate, channels);
  return 0;
}

int uac2_audio_start(void)
{
  if (!g_audio_dma.is_initialized)
    {
      /* 音声初期化が未完了の場合は要求を保留し、初期化完了時に開始する。
       * （無言で捨てるとポンプが永久停止するため、必ずログを出す）
       */
      g_audio_dma.start_pending = true;
      printf("[UAC2-AUDIO] Start requested before init, reserving\n");
      return 0;
    }

  if (g_audio_dma.is_playing)
    {
      return 0;
    }

  /* Rev84 (4): set is_playing only on success. A failed START must not
   * leave is_playing=true (that feeds a dead engine into an underrun
   * loop). Callers retry on next newstream; error propagates.
   */
  if (g_audio_dma.dev_fd >= 0)
    {
      int sret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_START, 0);
      int serr = (sret < 0) ? errno : 0;
      UAC2_TPRINTF("[UAC2-AUDIO] AUDIOIOC_START -> %d (errno=%d)\n", sret,
             serr);
      if (sret < 0)
        {
          /* NuttX ioctl returns -errno directly. */
          return sret;
        }
    }
  g_audio_dma.is_playing = true;
  sem_post(&g_audio_dma.pump_sem);
  UAC2_TPRINTF("[UAC2-AUDIO] CXD5247 DMA Playback started!\n");
  return 0;
}

int uac2_audio_stop(void)
{
  /* 保留中の開始要求も取り消す */
  g_audio_dma.start_pending = false;

  if (!g_audio_dma.is_playing)
    {
      return 0;
    }

  g_audio_dma.is_playing = false;
  if (g_audio_dma.dev_fd >= 0)
    {
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_STOP, 0);
    }
  uac2_ringbuf_init(&g_pcm_ring);
  printf("[UAC2-AUDIO] CXD5247 DMA Playback stopped.\n");
  return 0;
}

int uac2_audio_write(const void *buffer, size_t bytes)
{
  if (buffer == NULL || bytes == 0)
    {
      return 0;
    }

  /* P0: 8-byte stereo-frame guard (defense in depth; the driver already
   * filters, but DIAG feeders call here directly). Never accept a
   * non-multiple-of-8 length: it would misalign every subsequent frame.
   */
  if ((bytes & 7u) != 0)
    {
      return 0;
    }

  /* SMP hardening: drop pre-init packets. The ring/pump don't exist yet and
   * the pump sem was only just inited; early traffic is stale anyway
   * (stream restarts cleanly on first post-init packet via NEWSTREAM). */
  if (!g_audio_dma.is_initialized)
    {
      return 0;
    }

  /* Non-blocking ISR-safe write into lock-free ring buffer */
  uint32_t accepted = uac2_ringbuf_write(&g_pcm_ring, buffer, (uint32_t)bytes);
  g_audio_dma.iso_pkt_count++; /* ストリーム検出用（ポンプ側が50ms gap判定） */
  g_audio_dma.iso_sum_bytes += accepted;
  g_audio_dma.iso_sum_calls++;

  /* Rev81-diag (一時): intake capture。ISR負荷は64B memcpyのみ。
   * accepted<64の断片はroll更新のみ見送る (無菌試験では192B固定のはず)。 */
  if (accepted > 0)
    {
      const uint8_t *bp = (const uint8_t *)buffer;
      if (accepted >= 64u)
        {
          memcpy(g_audio_dma.cap_roll, bp + accepted - 64u, 64u);
        }
      if (g_audio_dma.cap_need_head)
        {
          uint32_t hn = (accepted >= 64u) ? 64u : accepted;
          memcpy(g_audio_dma.cap_head, bp, hn);
          g_audio_dma.cap_head_valid = true;
          g_audio_dma.cap_need_head = false;
        }
    }

#if 1 /* Rev48: USB起床を復活（Rev41で一時停止）。
       * 再生開始ERRとは無関係と確定済み。供給即応でpartial削減を狙う。
       * 起床postが飢餓・スピンの温床になるためsval<8でthrottle。
       */
  /* Wake up pump thread (throttled: backlog spins the pump at prio 150
   * and starves the main thread, killing console output during playback).
   */
  {
    int sval = 0;
    sem_getvalue(&g_audio_dma.pump_sem, &sval);
    if (sval < 8)
      {
        sem_post(&g_audio_dma.pump_sem);
      }
  }
#endif

  return (int)accepted;
}

void uac2_audio_set_volume(uint8_t volume_percent)
{
  if (volume_percent > 100u)
    {
      volume_percent = 100u;
    }
  g_audio_dma.volume_percent = volume_percent;

  if (g_audio_dma.dev_fd >= 0)
    {
      uint16_t gain = (uint16_t)((uint32_t)volume_percent * 10u);
      struct audio_caps_desc_s desc;
      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len = sizeof(struct audio_caps_s);
      desc.caps.ac_type = AUDIO_TYPE_FEATURE;
      desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
      desc.caps.ac_controls.hw[0] = gain;
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc);
    }
  UAC2_TPRINTF("[UAC2-AUDIO] Volume: %u%%\n", (unsigned)volume_percent);
}

void uac2_audio_set_mute(bool mute)
{
  g_audio_dma.mute = mute;
  if (g_audio_dma.dev_fd >= 0)
    {
      struct audio_caps_desc_s desc;
      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len = sizeof(struct audio_caps_s);
      desc.caps.ac_type = AUDIO_TYPE_FEATURE;
      desc.caps.ac_format.hw = AUDIO_FU_MUTE;
      desc.caps.ac_controls.hw[0] = mute ? 1u : 0u;
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc);
    }
  UAC2_TPRINTF("[UAC2-AUDIO] Mute: %s\n", mute ? "MUTED" : "UNMUTED");
}

/* 排出路の診断情報取得（1秒周期の状態表示用） */
void uac2_audio_get_stats(bool *playing, int *fd, uint32_t *enq,
                          uint32_t *deq, uint32_t *udr, uint32_t *rst,
                          int *freetop)
{
  if (playing) *playing = g_audio_dma.is_playing;
  if (fd)      *fd      = g_audio_dma.dev_fd;
  if (enq)     *enq     = g_audio_dma.enqueue_count;
  if (deq)     *deq     = g_audio_dma.dequeue_count;
  if (udr)     *udr     = g_audio_dma.underrun_count;
  if (rst)     *rst     = g_audio_dma.restart_needed;
  if (freetop) *freetop = g_audio_dma.free_top;
}

/* 一時診断：給電データ有無カウンタの取得 */
void uac2_audio_get_data_stats(uint32_t *data_chunks, uint32_t *silent_chunks)
{
  if (data_chunks)   *data_chunks   = g_audio_dma.data_chunks;
  if (silent_chunks) *silent_chunks = g_audio_dma.silent_chunks;
}

/* 一時診断：部分/空APBカウンタの取得 */
void uac2_audio_get_feed_stats(uint32_t *partial_chunks, uint32_t *empty_chunks)
{
  if (partial_chunks) *partial_chunks = g_audio_dma.partial_chunks;
  if (empty_chunks)   *empty_chunks   = g_audio_dma.empty_chunks;
}

/* 実給電リング統計の取得（[UAC2]表示用） */
void uac2_audio_get_ring_stats(uint32_t *underrun, uint32_t *overrun,
                               uint32_t *buffered)
{
  if (underrun) *underrun = g_pcm_ring.underrun_count;
  if (overrun)  *overrun  = g_pcm_ring.overrun_count;
  if (buffered) *buffered = uac2_ringbuf_available_read(&g_pcm_ring);
}

/* APBシーケンス追跡統計の取得（リプレイ/ドロップ判定用） */
void uac2_audio_get_seq_stats(uint32_t *dup, uint32_t *gap,
                              uint32_t *first_dup, uint32_t *first_gap)
{
  if (dup)       *dup       = g_audio_dma.dup_count;
  if (gap)       *gap       = g_audio_dma.gap_count;
  if (first_dup) *first_dup = g_audio_dma.first_dup_seq;
  if (first_gap) *first_gap = g_audio_dma.first_gap_seq;
}

/* サーボ統計の取得 */
void uac2_audio_get_servo_stats(uint32_t *dropped, uint32_t *dupped)
{
  if (dropped) *dropped = g_audio_dma.dropped_frames;
  if (dupped)  *dupped  = g_audio_dma.dupped_frames;
}

/* Rev76: async-feedback PI統計の取得（表示用） */
void uac2_audio_get_fb_stats(uint32_t *ff_q16, int32_t *err_b)
{
  if (ff_q16) *ff_q16 = g_fb_last_ff;
  if (err_b)  *err_b  = g_fb_last_err;
}

/* 一時診断：ドライバ通知メッセージ到着カウンタの取得 */
void uac2_audio_get_msg_stats(uint32_t *msg_underrun, uint32_t *msg_ioerror)
{
  if (msg_underrun) *msg_underrun = g_audio_dma.msg_underrun;
  if (msg_ioerror)  *msg_ioerror  = g_audio_dma.msg_ioerror;
}

/* オーディオクロックの有効状態（エンジン不動時の切り分け用） */
bool uac2_audio_clock_state(void)
{
  return cxd56_audio_clock_is_enabled();
}

void uac2_audio_get_diag_sample(uint32_t *raw, uint32_t *dst)
{
  if (raw) *raw = g_diag_raw_sample;
  if (dst) *dst = g_diag_dst_sample;
}

/* ビットパーフェクト検証用CRCの取得（表示時は最終xor済み値を返す） */
void uac2_audio_get_crc_stats(uint32_t *crc, uint32_t *bytes)
{
  if (crc)   *crc   = g_audio_dma.crc_val ^ 0xFFFFFFFFu;
  if (bytes) *bytes = g_audio_dma.crc_bytes;
}

/* 書込側監査カウンタの取得 */
void uac2_audio_get_iso_stats(uint32_t *sum_bytes, uint32_t *sum_calls)
{
  if (sum_bytes) *sum_bytes = g_audio_dma.iso_sum_bytes;
  if (sum_calls) *sum_calls = g_audio_dma.iso_sum_calls;
}

/* Rev81-diag (一時): intakeキャプチャの取得 (MONスレッドが変化時のみ表示) */
void uac2_audio_get_cap(const uint8_t **head, const uint8_t **frozen,
                        const uint8_t **roll, bool *hv, bool *fv)
{
  if (head)   *head   = g_audio_dma.cap_head;
  if (frozen) *frozen = g_audio_dma.cap_frozen;
  if (roll)   *roll   = g_audio_dma.cap_roll;
  if (hv)     *hv     = g_audio_dma.cap_head_valid;
  if (fv)     *fv     = g_audio_dma.cap_frozen_valid;
}

/* 音質確保：ポンプ内UART撤去に伴うMON表示用ゲッター（安価なvolatile読取のみ） */
void uac2_audio_get_mon_events(uint32_t *pump_wakes,
                               uint32_t *newstream, uint32_t *leftover,
                               uint32_t *fluke, uint32_t *revived,
                               uint32_t *failed, uint32_t *enq_fail)
{
  if (pump_wakes) *pump_wakes = g_audio_dma.pump_wakes;
  if (newstream)  *newstream  = g_audio_dma.mon_newstream;
  if (leftover)   *leftover   = g_audio_dma.mon_newstream_leftover;
  if (fluke)      *fluke      = g_audio_dma.mon_fluke_cancel;
  if (revived)    *revived    = g_audio_dma.mon_restart_revived;
  if (failed)     *failed     = g_audio_dma.mon_restart_failed;
  if (enq_fail)   *enq_fail   = g_audio_dma.mon_enq_fail;
}

/* Legacy entry points */
int uac2_audio_dma_init(void)
{
#if UAC2_AUDIO_DIAG_FORCE_48K16
  printf("[UAC2-AUDIO] DIAG: forcing 48kHz/16bit init\n");
  return uac2_audio_init(48000u, 16u, UAC2_AUDIO_DEFAULT_CHANNELS);
#else
  return uac2_audio_init(UAC2_AUDIO_DEFAULT_RATE,
                         UAC2_AUDIO_DEFAULT_DEPTH,
                         UAC2_AUDIO_DEFAULT_CHANNELS);
#endif
}

void uac2_audio_dma_start(void)
{
  uac2_audio_start();
}

void uac2_audio_dma_stop(void)
{
  uac2_audio_stop();
}
