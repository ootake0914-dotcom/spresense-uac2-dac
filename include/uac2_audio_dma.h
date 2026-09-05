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

int uac2_audio_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);
int uac2_audio_start(void);
int uac2_audio_stop(void);
int uac2_audio_write(const void *buffer, size_t bytes);
void uac2_audio_set_volume(uint8_t volume_percent);
void uac2_audio_set_mute(bool mute);

/* Legacy wrappers (signatures frozen by uac2_main.c) */

int uac2_audio_dma_init(void);
void uac2_audio_dma_start(void);
void uac2_audio_dma_stop(void);

#endif /* __UAC2_AUDIO_DMA_H */
