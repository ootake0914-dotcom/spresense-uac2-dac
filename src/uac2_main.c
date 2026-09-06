/**
 * @file uac2_main.c
 * @brief Main application entry for Spresense 192kHz/24bit USB DAC
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include "uac2.h"
#include "uac2_audio_dma.h"

extern int uac2_driver_register(void);
extern void uac2_driver_poll(void);
extern void uac2_get_status(bool *is_streaming, uint8_t *alt_setting, uint32_t *sample_rate,
                            uint32_t *underrun, uint32_t *overrun, uint32_t *buffered);
extern void uac2_dump_setup_logs(void);
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
    printf(" FW Rev68: MS OS 2.0 (WinUSB auto-bind MI_01)\n");
    printf(" Hardware: Sony CXD5602 + CXD5247 Audio Subsystem\n");
    printf(" Mode: Dedicated USB DAC Firmware (MIDI Engine Disabled)\n");
    printf("=======================================================\n");

    /* 1. Register USB driver FIRST so USB enumeration and control requests
     * (mmsys.cpl levels/formats) are answered immediately by EP0.
     * 一時診断ではUSB登録をスキップする。
     */
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

    /* 2. Initialize Audio Subsystem (CXD5247 DMA / /dev/pcm0) */
    printf("[UAC2] Initializing Audio Subsystem (CXD5247 DMA)...\n");
    fflush(stdout);
    ret = uac2_audio_dma_init();
    if (ret < 0) {
        printf("[UAC2] Warning: Audio subsystem init returned %d (continuing USB reg)...\n", ret);
    } else {
        printf("[UAC2] Audio subsystem initialized successfully!\n");
    }
    fflush(stdout);

#if UAC2_DIAG_NO_USB
    /* USBなし診断：SET_CONFIGが来ないため直接開始し、ポンプを周期起床させる */
    printf("[UAC2] DIAG NO_USB mode: starting audio directly\n");
    fflush(stdout);
    uac2_audio_start();
#endif

    printf("[UAC2] Connect Extension Board Micro-USB to PC.\n");
    printf("[UAC2] Monitoring USB Audio Status & Setup Logs...\n\n");
    fflush(stdout);

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
        /* Apply deferred Alt changes (streaming bring-up in task context) */
        uac2_driver_poll();

        /* Dump any received EP0 SETUP requests immediately */
#if !UAC2_SILENT_DIAG
        uac2_dump_setup_logs();
#endif
#endif

        /* Print periodic status every 1 second (10 x 100ms) */
#if !UAC2_SILENT_DIAG
        if (tick_100ms % 50 == 0) {
            bool is_streaming = false;
            uint8_t alt = 0;
            uint32_t sr = 0, underrun = 0, overrun = 0, buffered = 0;
            uac2_get_status(&is_streaming, &alt, &sr, &underrun, &overrun, &buffered);

            /* 排出路の診断情報（無音時の切り分け用） */
            bool aplaying = false;
            int afd = -9;
            uint32_t enq = 0, deq = 0, aud_udr = 0, rst = 0;
            int freetop = -9;
            uac2_audio_get_stats(&aplaying, &afd, &enq, &deq, &aud_udr, &rst, &freetop);

            const char *state_str = is_streaming ? "STREAMING (192kHz Active)" :
                                    (alt > 0 ? "ALT_SETTING_ACTIVE" : "STANDBY (Waiting Host Playback)");

            printf("[UAC2 #%lu] %s | Alt:%u | SR:%lu Hz | Buf:%lu B | Under:%lu | Over:%lu\n",
                   (unsigned long)(tick_100ms / 10), state_str, (unsigned)alt,
                   (unsigned long)sr, (unsigned long)buffered,
                   (unsigned long)underrun, (unsigned long)overrun);
            printf("[AUD #%lu] playing:%d fd:%d enq:%lu deq:%lu aud_udr:%lu rst:%lu freetop:%d clk:%d errcont:%lu\n",
                   (unsigned long)(tick_100ms / 10), (int)aplaying, afd,
                   (unsigned long)enq, (unsigned long)deq,
                   (unsigned long)aud_udr, (unsigned long)rst, freetop,
                   (int)uac2_audio_clock_state(), (unsigned long)g_cxd56_aud_errcont);
            {
              uint32_t pc = 0, ec = 0;
              uac2_audio_get_feed_stats(&pc, &ec);
              uint32_t dup = 0, gap = 0, fdup = 0, fgap = 0;
              uac2_audio_get_seq_stats(&dup, &gap, &fdup, &fgap);
              uint32_t svd = 0, svu = 0;
              uac2_audio_get_servo_stats(&svd, &svu);
              printf("[FEED #%lu] partial:%lu empty:%lu done:%lu erronly:%lu errdone:%lu dup:%lu gap:%lu fdup:%lu fgap:%lu svd:%lu svu:%lu\n",
                     (unsigned long)(tick_100ms / 10),
                     (unsigned long)pc, (unsigned long)ec,
                     (unsigned long)g_cxd56_aud_donecont,
                     (unsigned long)g_cxd56_aud_erronlycont,
                     (unsigned long)g_cxd56_aud_errdonecont,
                     (unsigned long)dup, (unsigned long)gap,
                     (unsigned long)fdup, (unsigned long)fgap,
                     (unsigned long)svd, (unsigned long)svu);
              printf("[EOGAP #%lu] g0:%lu g1:%lu g2:%lu g3:%lu g4:%lu g5:%lu g6:%lu g7:%lu g8:%lu g9:%lu\n",
                     (unsigned long)(tick_100ms / 10),
                     (unsigned long)g_cxd56_aud_eogap[0],
                     (unsigned long)g_cxd56_aud_eogap[1],
                     (unsigned long)g_cxd56_aud_eogap[2],
                     (unsigned long)g_cxd56_aud_eogap[3],
                     (unsigned long)g_cxd56_aud_eogap[4],
                     (unsigned long)g_cxd56_aud_eogap[5],
                     (unsigned long)g_cxd56_aud_eogap[6],
                     (unsigned long)g_cxd56_aud_eogap[7],
                     (unsigned long)g_cxd56_aud_eogap[8],
                     (unsigned long)g_cxd56_aud_eogap[9]);
            }
            {
              uint32_t dc = 0, sc = 0;
              uac2_audio_get_data_stats(&dc, &sc);
              uint32_t mu = 0, me = 0;
              uac2_audio_get_msg_stats(&mu, &me);
              printf("[DAT #%lu] data_chunks:%lu silent_chunks:%lu msg_udr:%lu msg_err:%lu\n",
                     (unsigned long)(tick_100ms / 10),
                     (unsigned long)dc, (unsigned long)sc,
                     (unsigned long)mu, (unsigned long)me);
            }
            fflush(stdout);
        }
#endif

        /* Periodic Hardware Register Dump every 3 seconds */
#if !UAC2_SILENT_DIAG
        if (tick_100ms % 100 == 0) {
            volatile uint32_t *reg_devcfg = (volatile uint32_t *)0x4E200400UL;
            volatile uint32_t *reg_devctl = (volatile uint32_t *)0x4E200404UL;
            volatile uint32_t *reg_devsts = (volatile uint32_t *)0x4E200408UL;
            volatile uint32_t *reg_devintr= (volatile uint32_t *)0x4E20040CUL;
            volatile uint32_t *reg_ep2ctl = (volatile uint32_t *)0x4E200240UL;
            volatile uint32_t *reg_ep2sts = (volatile uint32_t *)0x4E200244UL;
            volatile uint32_t *reg_busy   = (volatile uint32_t *)0x4E200808UL;

            printf("[REG] CFG=0x%08lx CTL=0x%08lx STS=0x%08lx INT=0x%08lx BUSY=0x%08lx | EP2CTL=0x%08lx EP2STS=0x%08lx\n",
                   (unsigned long)*reg_devcfg, (unsigned long)*reg_devctl, (unsigned long)*reg_devsts,
                   (unsigned long)*reg_devintr, (unsigned long)*reg_busy,
                   (unsigned long)*reg_ep2ctl, (unsigned long)*reg_ep2sts);

            /* 音声DMA状態（I2S1OUT系＝pcm0の実体。読取専用）。
             * 基準 0x0E300000＋オフセット (cxd56_audio_regdef.h)。
             */
            volatile uint32_t *au_adr   = (volatile uint32_t *)0x0E3010C0UL;
            volatile uint32_t *au_smpls = (volatile uint32_t *)0x0E3010C4UL;
            volatile uint32_t *au_cmd   = (volatile uint32_t *)0x0E3010C8UL;
            volatile uint32_t *au_chsel = (volatile uint32_t *)0x0E3010D0UL;
            volatile uint32_t *au_mon   = (volatile uint32_t *)0x0E3010D4UL;
            volatile uint32_t *au_istat = (volatile uint32_t *)0x0E301144UL;

            printf("[AUDREG] ADR=0x%08lx SMPL=0x%08lx CMD=0x%08lx CHSEL=0x%08lx MON=0x%08lx ISTAT=0x%08lx\n",
                   (unsigned long)*au_adr, (unsigned long)*au_smpls,
                   (unsigned long)*au_cmd, (unsigned long)*au_chsel,
                   (unsigned long)*au_mon, (unsigned long)*au_istat);
              if (g_cxd56_aud_errsnap_taken)
              {
                printf("[ERRSNAP] intbit=0x%08lx stat=0x%08lx mask=0x%08lx mon=0x%08lx smpls=0x%08lx addr=0x%08lx state=%lu errcont=%lu\n",
                       (unsigned long)g_cxd56_aud_errsnap[0],
                       (unsigned long)g_cxd56_aud_errsnap[1],
                       (unsigned long)g_cxd56_aud_errsnap[2],
                       (unsigned long)g_cxd56_aud_errsnap[3],
                       (unsigned long)g_cxd56_aud_errsnap[4],
                       (unsigned long)g_cxd56_aud_errsnap[5],
                       (unsigned long)g_cxd56_aud_errsnap[6],
                       (unsigned long)g_cxd56_aud_errcont);
              }
            if (g_cxd56_aud_errsnap2_taken)
              {
                printf("[ERRSNAP2] intbit=0x%08lx stat=0x%08lx mask=0x%08lx mon=0x%08lx smpls=0x%08lx addr=0x%08lx state=%lu errcont=%lu\n",
                       (unsigned long)g_cxd56_aud_errsnap2[0],
                       (unsigned long)g_cxd56_aud_errsnap2[1],
                       (unsigned long)g_cxd56_aud_errsnap2[2],
                       (unsigned long)g_cxd56_aud_errsnap2[3],
                       (unsigned long)g_cxd56_aud_errsnap2[4],
                       (unsigned long)g_cxd56_aud_errsnap2[5],
                       (unsigned long)g_cxd56_aud_errsnap2[6],
                       (unsigned long)g_cxd56_aud_errsnap2[7]);
              }
            fflush(stdout);
        }
#endif
    }

    return 0;
}
