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

extern int uac2_driver_register(void);
extern int uac2_audio_dma_init(void);
extern void uac2_driver_poll(void);
extern void uac2_get_status(bool *is_streaming, uint8_t *alt_setting, uint32_t *sample_rate,
                            uint32_t *underrun, uint32_t *overrun, uint32_t *buffered);
extern void uac2_dump_setup_logs(void);

int main(int argc, char *argv[])
{
    printf("\n=======================================================\n");
    printf(" Spresense 192kHz / 24-bit USB Audio Class 2.0 (UAC2) DAC\n");
    printf(" FW Rev22: Alt 0 Single-Setting Streaming (Hardware STALL Bypass)\n");
    printf(" Hardware: Sony CXD5602 + CXD5247 Audio Subsystem\n");
    printf(" Mode: Dedicated USB DAC Firmware (MIDI Engine Disabled)\n");
    printf("=======================================================\n");

    /* 1. Register USB driver FIRST so USB enumeration and control requests
     * (mmsys.cpl levels/formats) are answered immediately by EP0.
     */
    printf("[UAC2] Registering USB Audio Class 2.0 driver...\n");
    fflush(stdout);
    int ret = uac2_driver_register();
    if (ret < 0) {
        printf("[UAC2] Failed to register USB Audio Class driver: %d\n", ret);
        fflush(stdout);
        return -1;
    }
    printf("[UAC2] USB Driver registered successfully!\n");
    fflush(stdout);

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

    printf("[UAC2] Connect Extension Board Micro-USB to PC.\n");
    printf("[UAC2] Monitoring USB Audio Status & Setup Logs...\n\n");
    fflush(stdout);

    uint32_t tick_100ms = 0;
    while (1) {
        usleep(100000); /* 100ms polling */
        tick_100ms++;

        /* Apply deferred Alt changes (streaming bring-up in task context) */
        uac2_driver_poll();

        /* Dump any received EP0 SETUP requests immediately */
        uac2_dump_setup_logs();

        /* Print periodic status every 1 second (10 x 100ms) */
        if (tick_100ms % 10 == 0) {
            bool is_streaming = false;
            uint8_t alt = 0;
            uint32_t sr = 0, underrun = 0, overrun = 0, buffered = 0;
            uac2_get_status(&is_streaming, &alt, &sr, &underrun, &overrun, &buffered);

            const char *state_str = is_streaming ? "STREAMING (192kHz Active)" :
                                    (alt > 0 ? "ALT_SETTING_ACTIVE" : "STANDBY (Waiting Host Playback)");

            printf("[UAC2 #%lu] %s | Alt:%u | SR:%lu Hz | Buf:%lu B | Under:%lu | Over:%lu\n",
                   (unsigned long)(tick_100ms / 10), state_str, (unsigned)alt,
                   (unsigned long)sr, (unsigned long)buffered,
                   (unsigned long)underrun, (unsigned long)overrun);
            fflush(stdout);
        }

        /* Periodic Hardware Register Dump every 3 seconds */
        if (tick_100ms % 30 == 0) {
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
            fflush(stdout);
        }
    }

    return 0;
}
