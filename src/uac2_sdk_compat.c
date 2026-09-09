/**
 * @file uac2_sdk_compat.c
 * @brief Stock-SDK compatibility stubs for private SDK diagnostics.
 *
 * The original firmware was developed against a privately patched
 * cxd56_audio_dma.c that exported live DMA-error snapshot globals
 * (g_cxd56_aud_*). Stock SDK (sonydevworld/spresense) has no such
 * symbols, so referencing them would fail at link time.
 *
 * All symbols below are WEAK definitions: they satisfy the linker on
 * stock SDK (telemetry reads zero / "not taken"), and a patched SDK
 * that defines them strongly automatically overrides these stubs.
 */

#include <stdint.h>

volatile uint32_t g_cxd56_aud_errsnap[8] __attribute__((weak));
volatile int g_cxd56_aud_errsnap_taken __attribute__((weak));
volatile uint32_t g_cxd56_aud_errcont __attribute__((weak));
volatile uint32_t g_cxd56_aud_errsnap2[8] __attribute__((weak));
volatile int g_cxd56_aud_errsnap2_taken __attribute__((weak));
volatile uint32_t g_cxd56_aud_donecont __attribute__((weak));
volatile uint32_t g_cxd56_aud_erronlycont __attribute__((weak));
volatile uint32_t g_cxd56_aud_errdonecont __attribute__((weak));
volatile uint32_t g_cxd56_aud_eogap[10] __attribute__((weak));
