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
extern void uac2_get_status(bool *is_streaming, uint8_t *alt_setting, uint32_t *sample_rate,
                            uint32_t *underrun, uint32_t *overrun, uint32_t *buffered);

int main(int argc, char *argv[])
{
    printf("\n=======================================================\n");
    printf(" Spresense 192kHz / 24-bit USB Audio Class 2.0 (UAC2) DAC\n");
    printf(" Hardware: Sony CXD5602 + CXD5247 Audio Subsystem\n");
    printf(" Mode: Dedicated USB DAC Firmware (MIDI Engine Disabled)\n");
    printf("=======================================================\n");

    int ret = uac2_audio_dma_init();
    if (ret < 0) {
        printf("[UAC2] Failed to initialize audio subsystem: %d\n", ret);
        return -1;
    }

    ret = uac2_driver_register();
    if (ret < 0) {
        printf("[UAC2] Failed to register USB Audio Class driver: %d\n", ret);
        return -1;
    }

    printf("[UAC2] Driver registered successfully!\n");
    printf("[UAC2] Connect Extension Board Micro-USB to PC.\n");
    printf("[UAC2] Monitoring USB Audio Status (1 sec interval)...\n\n");

    uint32_t loop_count = 0;
    while (1) {
        sleep(1);
        loop_count++;

        bool is_streaming = false;
        uint8_t alt = 0;
        uint32_t sr = 0, underrun = 0, overrun = 0, buffered = 0;
        uac2_get_status(&is_streaming, &alt, &sr, &underrun, &overrun, &buffered);

        const char *state_str = is_streaming ? "STREAMING (192kHz Active)" :
                                (alt > 0 ? "ALT_SETTING_ACTIVE" : "STANDBY (Waiting Host Playback)");

        printf("[UAC2 #%lu] %s | Alt:%u | SR:%lu Hz | Buf:%lu B | Under:%lu | Over:%lu\n",
               (unsigned long)loop_count, state_str, (unsigned)alt,
               (unsigned long)sr, (unsigned long)buffered,
               (unsigned long)underrun, (unsigned long)overrun);
    }

    return 0;
}
