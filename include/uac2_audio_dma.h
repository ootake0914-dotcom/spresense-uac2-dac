/**
 * @file uac2_audio_dma.h
 * @brief Phase 3: CXD5247 audio output subsystem (192kHz/24bit DAC playback)
 *
 * Data flow:
 *   USB ISO OUT (Phase 1, uac2_driver.c)
 *     -> uac2_audio_write()            [ISR-safe, non-blocking push]
 *     -> private SPSC ring buffer     [64KB, ~42ms @192k/stereo/32-bit slot]
 *     -> pump thread                  [4096B chunks, zero-fill on underrun]
 *     -> /dev/pcm0 (NuttX audio upper-half + CXD56 SPK lower-half)
 *     -> CXD5247 DAC -> headphone out
 *
 * Legacy entry points (uac2_audio_dma_init/start/stop, used by uac2_main.c)
 * are kept with identical signatures and forward to the new API using the
 * firmware default format (192kHz / 24-bit / stereo).
 */

#ifndef __UAC2_AUDIO_DMA_H
#define __UAC2_AUDIO_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Firmware default format (matches UAC2 Alt-1 descriptor + CXD5247 HIRES) */

#define UAC2_AUDIO_DEFAULT_RATE     192000u
#define UAC2_AUDIO_DEFAULT_DEPTH    24u
#define UAC2_AUDIO_DEFAULT_CHANNELS 2u

/* Default digital volume in percent (applied at init) */

#define UAC2_AUDIO_DEFAULT_VOLUME   80u

/* Pump transfer chunk: matches CXD56_AUDIO_BUFFER_SIZE default (4096).
 * At 192kHz stereo 32-bit slot this is ~2.67ms of audio.
 */

#define UAC2_AUDIO_PUMP_CHUNK       4096u

/* Rev84: clock-drift servo (drop/dup correction) master switch.
 *   1 = enabled (product default): proportional frame drops above 64KB
 *       and dup-trim below 4KB absorb crystal offset indefinitely.
 *   0 = pure passthrough (bit-perfect audit mode): no sample is ever
 *       dropped or duplicated by the pump; overruns still counted by
 *       the ring. svd/svu freeze at 0, proving non-intervention.
 * TEMP-AUDIT: 0 for the Rev84 audit build (revert to 1 after).
 */
#define UAC2_SERVO_ENABLE           1

int uac2_audio_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);
int uac2_audio_start(void);
int uac2_audio_stop(void);
int uac2_audio_write(const void *buffer, size_t bytes);
void uac2_audio_set_volume(uint8_t volume_percent);
void uac2_audio_set_mute(bool mute);

/* 排出路の診断情報取得（1秒周期の状態表示用。無音時の切り分けに使用） */
void uac2_audio_get_stats(bool *playing, int *fd, uint32_t *enq,
                          uint32_t *deq, uint32_t *udr, uint32_t *rst,
                          int *freetop);

/* 一時診断用：給電データ有無カウンタの取得 */
void uac2_audio_get_data_stats(uint32_t *data_chunks, uint32_t *silent_chunks);

/* 一時診断用：部分/空APBカウンタの取得 */
void uac2_audio_get_feed_stats(uint32_t *partial_chunks, uint32_t *empty_chunks);

/* 実給電リング統計の取得（[UAC2]表示用。死にバッファ参照の置換） */
void uac2_audio_get_ring_stats(uint32_t *underrun, uint32_t *overrun,
                               uint32_t *buffered);

/* APBシーケンス追跡統計の取得（リプレイ/ドロップ判定用） */
void uac2_audio_get_seq_stats(uint32_t *dup, uint32_t *gap,
                              uint32_t *first_dup, uint32_t *first_gap);

/* サーボ統計の取得 */
void uac2_audio_get_servo_stats(uint32_t *dropped, uint32_t *dupped);

/* Rev76: async-feedback PI統計の取得（表示用） */
void uac2_audio_get_fb_stats(uint32_t *ff_q16, int32_t *err_b);

/* 一時診断用：ドライバ通知メッセージ到着カウンタの取得 */
void uac2_audio_get_msg_stats(uint32_t *msg_underrun, uint32_t *msg_ioerror);

/* オーディオクロックの有効状態（エンジン不動時の切り分け用） */
bool uac2_audio_clock_state(void);

/* 一時診断用：直近サンプルの生値（raw / dst）取得 */
void uac2_audio_get_diag_sample(uint32_t *raw, uint32_t *dst);

/* ビットパーフェクト検証用CRCの取得（表示時は最終xor済み） */
void uac2_audio_get_crc_stats(uint32_t *crc, uint32_t *bytes);

/* 書込側監査カウンタの取得 */
void uac2_audio_get_iso_stats(uint32_t *sum_bytes, uint32_t *sum_calls);

/* Rev81-diag (一時): intakeキャプチャの取得 (MONスレッドが変化時のみ表示) */
void uac2_audio_get_cap(const uint8_t **head, const uint8_t **frozen,
                        const uint8_t **roll, bool *hv, bool *fv);

/* 音質確保：ポンプ内イベント計数の取得（MONスレッド表示用） */
void uac2_audio_get_mon_events(uint32_t *pump_wakes,
                               uint32_t *newstream, uint32_t *leftover,
                               uint32_t *fluke, uint32_t *revived,
                               uint32_t *failed, uint32_t *enq_fail);

/* Rev76: async-feedbackペイロード書込み（uac2_driver.cが実装。
 * pumpスレッドが1ms毎にPI出力Q16.16を渡す）
 */
void uac2_feedback_update(uint32_t ff_q16);

/* Rev76: paced feedback submitter (uac2_driver.cが実装。
 * pumpスレッドが1ms毎に呼ぶ。streaming中かつ送出中でなければ4B送信）
 */
void uac2_feedback_poll(void);

/* Legacy wrappers (signatures frozen by uac2_main.c) */

int uac2_audio_dma_init(void);
void uac2_audio_dma_start(void);
void uac2_audio_dma_stop(void);

#endif /* __UAC2_AUDIO_DMA_H */
