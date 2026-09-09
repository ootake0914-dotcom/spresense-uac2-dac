/**
 * @file uac2_main.c
 * @brief Main application entry for Spresense 192kHz/24bit USB DAC
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include "uac2.h"
#include "uac2_audio_dma.h"
#include "uac2_monitor.h"

extern int uac2_driver_register(void);
extern void uac2_driver_poll(void);
extern void uac2_get_status(bool *is_streaming, uint8_t *alt_setting, uint32_t *sample_rate,
                            uint32_t *underrun, uint32_t *overrun, uint32_t *buffered);
/* SDK側の一時診断スナップショット（DMA ERROR発火瞬間の生値） */
extern volatile uint32_t g_cxd56_aud_errsnap[8];
extern volatile int g_cxd56_aud_errsnap_taken;
extern volatile uint32_t g_cxd56_aud_errcont;
extern volatile uint32_t g_cxd56_aud_errsnap2[8];
extern volatile int g_cxd56_aud_errsnap2_taken;
extern volatile uint32_t g_cxd56_aud_donecont;
extern volatile uint32_t g_cxd56_aud_erronlycont;
extern volatile uint32_t g_cxd56_aud_errdonecont;
extern volatile uint32_t g_cxd56_aud_eogap[10];

/* 一時診断：1にするとUSB登録をスキップし音声のみで動作する。
 * USBなしでdeqが回ることを確認済みのため0に戻す。
 */
#define UAC2_DIAG_NO_USB 0

int main(int argc, char *argv[])
{
    int ret = 0;

    printf("\n=======================================================\n");
    printf(" Spresense 192kHz / 24-bit USB Audio Class 2.0 (UAC2) DAC\n");
    printf(" FW Rev84: mono-dt PI + servo switch + stream states + atomics\n");
    printf(" Hardware: Sony CXD5602 + CXD5247 Audio Subsystem\n");
    printf(" Mode: Dedicated USB DAC Firmware (MIDI Engine Disabled)\n");
    printf(" Audio path: USB-ISR -> ring -> pump(CPU0, no UART) -> DMA\n");
    printf(" Telemetry: MON thread (low prio, sub-core when SMP)\n");
    printf("=======================================================\n");

    /* Rev78 (SMP death fix): AUDIO FIRST, USB SECOND.
     * Proven by replug test: SET_CONFIG arriving DURING audio init wedges
     * SMP deterministically (4/4 deaths in the init tail), while the same
     * SET_CONFIG after init is safe (STREAMING sustained). Previously USB
     * was registered first for fast enumeration; the ~1s delay costs
     * nothing against a dead board. With audio fully initialized
     * (pump thread up, sems valid, DMA pre-fed), the host can enumerate
     * and stream immediately without ever hitting a half-built pipeline.
     * 一時診断ではUSB登録をスキップする。
     */

    /* 1. Initialize Audio Subsystem (CXD5247 DMA / /dev/pcm0) */
    printf("[UAC2] Initializing Audio Subsystem (CXD5247 DMA)...\n");
    fflush(stdout);
    ret = uac2_audio_dma_init();
    if (ret < 0) {
        /* P0: abort init. Continuing to USB registration with a dead audio
         * pipeline accepts host traffic into an undrained ring (overrun
         * storm) and serves a non-functional DAC. Pump-thread spawn
         * failure already reports its errno above.
         */
        printf("[UAC2] FATAL: Audio subsystem init failed (%d), aborting.\n", ret);
        fflush(stdout);
        return -1;
    } else {
        printf("[UAC2] Audio subsystem initialized successfully!\n");
    }
    fflush(stdout);

    /* 2. Register USB driver AFTER audio is ready (see above) */
#if UAC2_DIAG_NO_USB
    printf("[UAC2] DIAG NO_USB mode: skipping USB driver registration\n");
    fflush(stdout);
#else
    printf("[UAC2] Registering USB Audio Class 2.0 driver...\n");
    fflush(stdout);
    ret = uac2_driver_register();
    if (ret < 0) {
        printf("[UAC2] Failed to register USB Audio Class driver: %d\n", ret);
        fflush(stdout);
        return -1;
    }
    printf("[UAC2] USB Driver registered successfully!\n");
    fflush(stdout);
#endif

#if UAC2_DIAG_NO_USB
    /* USBなし診断：SET_CONFIGが来ないため直接開始し、ポンプを周期起床させる */
    printf("[UAC2] DIAG NO_USB mode: starting audio directly\n");
    fflush(stdout);
    uac2_audio_start();
#endif

    printf("[UAC2] Connect Extension Board Micro-USB to PC.\n");
    printf("[UAC2] Monitoring USB Audio Status & Setup Logs...\n\n");
    fflush(stdout);

    /* 音質確保：起動後の定常UARTはMONスレッド専用にする。ここでは起動のみ。 */
    if (uac2_monitor_init() != 0)
      {
        printf("[UAC2] Warning: monitor thread init failed (telemetry quiet)\n");
        fflush(stdout);
      }

    uint32_t tick_100ms = 0;
    while (1) {
        usleep(100000); /* 100ms polling */
        tick_100ms++;

#if UAC2_DIAG_NO_USB
        /* USBなし診断：自前正弦波（440Hz@48kHz/16bit/stereo）を100ms分給電する。
         * static領域使用（mainスタック4KBに収まらないため）。
         */
        {
          static uint8_t feedbuf[4800u * 4u];
          static const int16_t sintab[64] = {
            0, 1568, 3121, 4643, 6124, 7541, 8880, 10126,
            11276, 12316, 13238, 14033, 14696, 15221, 15608, 15858,
            16000, 15858, 15608, 15221, 14696, 14033, 13238, 12316,
            11276, 10126, 8880, 7541, 6124, 4643, 3121, 1568,
            0, -1568, -3121, -4643, -6124, -7541, -8880, -10126,
            -11276, -12316, -13238, -14033, -14696, -15221, -15608, -15858,
            -16000, -15858, -15608, -15221, -14696, -14033, -13238, -12316,
            -11276, -10126, -8880, -7541, -6124, -4643, -3121, -1568
          };
          static uint32_t sine_phase = 0;
          int16_t *p = (int16_t *)feedbuf;
          for (uint32_t f = 0; f < 4800u; f++)
            {
              int16_t s = sintab[(sine_phase >> 26) & 63u];
              sine_phase += 39370534u; /* 440Hz @48kHz */
              *p++ = s; /* L */
              *p++ = s; /* R */
            }
          uac2_audio_write(feedbuf, sizeof(feedbuf));
        }
#else
        /* Apply deferred Alt changes (streaming bring-up in task context).
         * Hardware bring-up only; no UART here (rare Alt-change logs stay
         * inside uac2_driver_poll and fire only on transitions).
         */
        uac2_driver_poll();

        /* NOTE: EP0 setup-log drain moved to MON thread (sub-core).
         * Main/USB path never touches UART in steady state. */
#endif

        /* Telemetry snapshot: cheap reads only, formatting on MON thread.
         * Cadence preserved (50 ticks = status batch, 100 ticks = +regs). */
        if (tick_100ms % 50 == 0) {
            struct uac2_mon_snapshot_s snap;
            memset(&snap, 0, sizeof(snap));
            snap.tick_no = tick_100ms / 10;

            uac2_get_status(&snap.is_streaming, &snap.alt, &snap.sample_rate,
                            &snap.underrun, &snap.overrun, &snap.buffered);
            uac2_audio_get_stats(&snap.aplaying, &snap.afd, &snap.enq,
                                 &snap.deq, &snap.aud_udr, &snap.rst,
                                 &snap.freetop);
            snap.clk = (int)uac2_audio_clock_state();
            snap.errcont = g_cxd56_aud_errcont;

            uac2_audio_get_feed_stats(&snap.partial, &snap.empty);
            uac2_audio_get_seq_stats(&snap.dup, &snap.gap,
                                     &snap.fdup, &snap.fgap);
            uac2_audio_get_servo_stats(&snap.svd, &snap.svu);
            uac2_audio_get_fb_stats(&snap.fbff, &snap.fberr);
            snap.done = g_cxd56_aud_donecont;
            snap.erronly = g_cxd56_aud_erronlycont;
            snap.errdone = g_cxd56_aud_errdonecont;
            for (int i = 0; i < 10; i++)
              {
                snap.eogap[i] = g_cxd56_aud_eogap[i];
              }
            uac2_audio_get_data_stats(&snap.dc, &snap.sc);
            uac2_audio_get_msg_stats(&snap.mu, &snap.me);
            uac2_audio_get_diag_sample(&snap.r_smp, &snap.d_smp);
            uac2_audio_get_crc_stats(&snap.crc_val, &snap.crc_bytes);
            uac2_audio_get_iso_stats(&snap.iso_sum_bytes, &snap.iso_sum_calls);
            uac2_audio_get_mon_events(&snap.pump_wakes,
                                      &snap.mon_newstream,
                                      &snap.mon_newstream_leftover,
                                      &snap.mon_fluke_cancel,
                                      &snap.mon_restart_revived,
                                      &snap.mon_restart_failed,
                                      &snap.mon_enq_fail);

            if (tick_100ms % 100 == 0) {
                snap.has_reg = true;
                snap.reg_devcfg  = *(volatile uint32_t *)0x4E200400UL;
                snap.reg_devctl  = *(volatile uint32_t *)0x4E200404UL;
                snap.reg_devsts  = *(volatile uint32_t *)0x4E200408UL;
                snap.reg_devintr = *(volatile uint32_t *)0x4E20040CUL;
                snap.reg_ep2ctl  = *(volatile uint32_t *)0x4E200240UL;
                snap.reg_ep2sts  = *(volatile uint32_t *)0x4E200244UL;
                snap.reg_busy    = *(volatile uint32_t *)0x4E200808UL;

                snap.au_adr   = *(volatile uint32_t *)0x0E3010C0UL;
                snap.au_smpls = *(volatile uint32_t *)0x0E3010C4UL;
                snap.au_cmd   = *(volatile uint32_t *)0x0E3010C8UL;
                snap.au_chsel = *(volatile uint32_t *)0x0E3010D0UL;
                snap.au_mon   = *(volatile uint32_t *)0x0E3010D4UL;
                snap.au_istat = *(volatile uint32_t *)0x0E301144UL;

                if (g_cxd56_aud_errsnap_taken)
                  {
                    snap.has_errsnap = true;
                    for (int i = 0; i < 8; i++)
                      {
                        snap.errsnap[i] = g_cxd56_aud_errsnap[i];
                      }
                  }
                if (g_cxd56_aud_errsnap2_taken)
                  {
                    snap.has_errsnap2 = true;
                    for (int i = 0; i < 8; i++)
                      {
                        snap.errsnap2[i] = g_cxd56_aud_errsnap2[i];
                      }
                  }
            }

            uac2_monitor_push(&snap);
        }
    }

    return 0;
}
