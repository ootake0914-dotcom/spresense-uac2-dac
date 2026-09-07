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

#define UAC2_AUDIO_DEV_PATH         "/dev/audio/pcm0"
#define UAC2_AUDIO_DEV_ALT          "/dev/pcm0"

/* 製品形式に戻す（192kHz/24bit）。48k/16bit診断は完了したため無効化。
 * 一時診断スイッチ：1にすると正規形式48kHz/16bitで初期化する。
 */
#define UAC2_AUDIO_DIAG_FORCE_48K16 0

#define UAC2_AUDIO_NUM_BUFFERS      16
#define UAC2_AUDIO_BUFFER_SIZE      2048   /* 256 frames @ 192kHz stereo 32-bit = 1.33ms */
/* ポンプスレッドのスタック：chunk[2048]＋割込みネストに備えて余裕を持つ */
#define UAC2_PUMP_STACKSIZE         8192

/* Slot bitwidth: 24-bit PCM in 32-bit container */
#define UAC2_SLOTWIDTH_32           32u
#define UAC2_SLOTWIDTH_16           16u

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
};

static struct uac2_audio_dma_s g_audio_dma;
static Uac2RingBuffer g_pcm_ring;

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
            printf("[UAC2-AUDIO] MSG UNDERRUN #%lu\n",
                   (unsigned long)g_audio_dma.msg_underrun);
            fflush(stdout);
            g_audio_dma.restart_needed = 1;
            break;
          case AUDIO_MSG_IOERROR:
            g_audio_dma.msg_ioerror++;
            printf("[UAC2-AUDIO] MSG IOERROR #%lu\n",
                   (unsigned long)g_audio_dma.msg_ioerror);
            fflush(stdout);
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
  /* 2KB作業域はstatic化（8KBポンプスタックの溢れ保険） */
  static uint8_t chunk[UAC2_AUDIO_BUFFER_SIZE];

  printf("[UAC2-AUDIO] Pump thread started (priority 150)\n");

  /* 一時診断：ポンプ自身の定期報告（生存確認＋排出監視用） */
  uint32_t pump_wakes = 0;
  /* ストリーム検出（ESP new_play儀式）：50ms以上の無到着gap後の初到着を
   * 新ストリームとし、前ストリーム残渣を捨てて位相を確定させる。
   * tail=headの1ストアのみ（memset掃除は競合・遅延のため不可）。
   */
  uint32_t last_pc = 0;
  uint32_t quiet_wakes = 0;
  uint32_t stream_seq = 0;

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
      pump_wakes++;

      /* 計測printは壁時計1秒に1回まで（TX負荷軽減。+40秒沈黙対策）。
       * 再生中はUSB起床でwakeが爆発するためwake数基準は不可。
       * SILENT_DIAG時は全面停止（UART wedge切り分け用）。
       */
#if !UAC2_SILENT_DIAG
      {
        struct timespec nowts;
        static time_t last_pump_sec = 0;
        clock_gettime(CLOCK_REALTIME, &nowts);
        if (nowts.tv_sec != last_pump_sec)
          {
            last_pump_sec = nowts.tv_sec;
            int sval = -99;
            sem_getvalue(&g_audio_dma.pump_sem, &sval);
            printf("[PUMP] wakes=%lu enq=%lu deq=%lu freetop=%d sem=%d avail=%lu\n",
                   (unsigned long)pump_wakes,
                   (unsigned long)g_audio_dma.enqueue_count,
                   (unsigned long)g_audio_dma.dequeue_count,
                   g_audio_dma.free_top, sval,
                   (unsigned long)uac2_ringbuf_available_read(&g_pcm_ring));
            fflush(stdout);
          }
      }
#endif

      if (!g_audio_dma.is_playing || g_audio_dma.dev_fd < 0)
        {
          continue;
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
                    g_pcm_ring.tail = g_pcm_ring.head;
                  }
                stream_seq++;
                printf("[UAC2-AUDIO] NEWSTREAM #%lu leftover=%lu (mod8=%lu)\n",
                       (unsigned long)stream_seq, (unsigned long)left,
                       (unsigned long)(left & 7u));
                fflush(stdout);
              }
            last_pc = pc;
            quiet_wakes = 0;
          }
        else if (quiet_wakes < 1000000u)
          {
            quiet_wakes++;
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
          uint32_t g0 = g_audio_dma.dequeue_count;
          usleep(50000);
          drain_audio_msgs(g_audio_dma.mq);
          uint32_t g1 = g_audio_dma.dequeue_count;
          usleep(50000);
          drain_audio_msgs(g_audio_dma.mq);
          uint32_t g2 = g_audio_dma.dequeue_count;
          if ((g1 - g0) >= 10 && (g2 - g1) >= 10)
            {
              printf("[UAC2-AUDIO] RESTART FLUKE-cancel (deq %lu+%lu, engine alive)\n",
                     (unsigned long)(g1 - g0), (unsigned long)(g2 - g1));
              fflush(stdout);
            }
          else
            {
          uint32_t deq_entry = g_audio_dma.dequeue_count;
          uint32_t comp_entry = g_audio_dma.msg_complete;
          int stopped_once = 0;
          int revived = 0;
          int consec = 0;
          for (int t = 0; t < 300; t++)
            {
              drain_audio_msgs(g_audio_dma.mq);
              if (!stopped_once &&
                  (g_audio_dma.msg_complete > comp_entry ||
                   g_audio_dma.dequeue_count > deq_entry))
                {
                  int stop_ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_STOP, 0);
                  stopped_once = 1;
                  printf("[UAC2-AUDIO] RESTART stop issued ret=%d\n", stop_ret);
                  fflush(stdout);
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
              uint32_t deq0 = g_audio_dma.dequeue_count;
              usleep(3000);
              drain_audio_msgs(g_audio_dma.mq);
              uint32_t deq1 = g_audio_dma.dequeue_count;
              if ((t % 50) == 0 || (deq1 - deq0) >= 2)
                {
                  printf("[UAC2-AUDIO] RESTART try=%d refill=%d free=%d start=%d deq_adv=%lu stop=%d\n",
                         t, refill, g_audio_dma.free_top, start_ret,
                         (unsigned long)(deq1 - deq0), stopped_once);
                  fflush(stdout);
                }
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
          printf("[UAC2-AUDIO] RESTART %s\n", revived ? "REVIVED" : "FAILED");
          fflush(stdout);
            } /* end else (genuine flat death) */
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
          /* クロックドリフトサーボ（dead-band式）：ホストとエンジンの
           * 数十ppm差で水位が∼20分で壁に到達し慢性破綻する。
           * HIGH(192K)超で1frame/8wakeを捨て、LOW(2APB未満からの
           * フルtake時)で1frame/8takeを直前frame反復で嵩増しする。
           * どちらも±0.05%・単発frameで不可聴。水位を有界に保つ。
           */
          if (avail > 96u * 1024u)
            {
              static uint8_t drop8[8];
              g_audio_dma.servo_tick++;
              if ((g_audio_dma.servo_tick & 7u) == 0)
                {
                  uac2_ringbuf_read(&g_pcm_ring, drop8, 8);
                  g_audio_dma.dropped_frames++;
                  avail = uac2_ringbuf_available_read(&g_pcm_ring);
                }
            }
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
           */
          int dup_this = 0;
          if (n == UAC2_AUDIO_BUFFER_SIZE && avail < 4096u)
            {
              g_audio_dma.servo_tick++;
              if ((g_audio_dma.servo_tick & 7u) == 0)
                {
                  dup_this = 1;
                }
            }
          if (n == 0)
            {
              g_audio_dma.empty_chunks++;
            }
          else if (n < UAC2_AUDIO_BUFFER_SIZE)
            {
              g_audio_dma.partial_chunks++;
            }
          if (dup_this)
            {
              /* 直前frame反復を先頭に挿入し、新規消費を2040Bに抑える。
               * APBは2048Bのまま（ペイロード破壊なし・サイズ不変）。
               */
              memcpy(chunk, g_audio_dma.histframe, 8);
              uac2_ringbuf_read(&g_pcm_ring, chunk + 8,
                                UAC2_AUDIO_BUFFER_SIZE - 8);
              g_audio_dma.dupped_frames++;
            }
          else
            {
              if (n > 0)
                {
                  uac2_ringbuf_read(&g_pcm_ring, chunk, n);
                }
              if (n < UAC2_AUDIO_BUFFER_SIZE)
                {
                  memset(chunk + n, 0, UAC2_AUDIO_BUFFER_SIZE - n);
                }
            }
          /* Rev69: 24-bit Alignment Fix.
           * Linux sends LSB-aligned 24-bit in 32-bit slot (0x00XXXXXX).
           * CXD5602/CXD5247 hardware DAC expects MSB-aligned (0xXXXXXX00).
           * Shift left by 8 bits to restore sign bit and full 24-bit dynamic range.
           */
          {
            const uint32_t *src32 = (const uint32_t *)chunk;
            uint32_t *dst32 = (uint32_t *)apb->samp;
            for (uint32_t i = 0; i < UAC2_AUDIO_BUFFER_SIZE / 4; i++)
              {
                dst32[i] = src32[i] << 8;
              }
          }
          memcpy(g_audio_dma.histframe, chunk + UAC2_AUDIO_BUFFER_SIZE - 8, 8);

          /* 一時診断：チャンク内の実データ有無を監査する */
          {
            const uint32_t *w = (const uint32_t *)apb->samp;
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < UAC2_AUDIO_BUFFER_SIZE / 4; i += 4)
              {
                if (w[i] != 0u)
                  {
                    nonzero = 1;
                    break;
                  }
              }
            if (nonzero)
              {
                g_audio_dma.data_chunks++;
              }
            else
              {
                g_audio_dma.silent_chunks++;
              }
          }
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
              /* 投入失敗は無言で捨てない（無音時の切り分け情報になるため間引き表示） */
              static volatile uint32_t enq_fail_count = 0;
              enq_fail_count++;
              if (enq_fail_count == 1 || (enq_fail_count % 200) == 0)
                {
                  printf("[UAC2-AUDIO] ENQUEUEBUFFER failed #%lu ret=%d errno=%d\n",
                         (unsigned long)enq_fail_count, ret, errno);
                  fflush(stdout);
                }
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

  /* Set initial volume (100% / 0dB) */
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

  sem_init(&g_audio_dma.pump_sem, 0, 0);
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
      printf("[UAC2-AUDIO] ERROR: Failed to create pump thread: %d\n", ret);
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

  g_audio_dma.is_playing = true;
  if (g_audio_dma.dev_fd >= 0)
    {
      int sret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_START, 0);
      UAC2_TPRINTF("[UAC2-AUDIO] AUDIOIOC_START -> %d (errno=%d)\n", sret,
             (sret < 0) ? errno : 0);
    }
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

  /* Non-blocking ISR-safe write into lock-free ring buffer */
  uint32_t accepted = uac2_ringbuf_write(&g_pcm_ring, buffer, (uint32_t)bytes);
  g_audio_dma.iso_pkt_count++; /* ストリーム検出用（ポンプ側が50ms gap判定） */

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
