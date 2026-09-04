/**
 * @file uac2_main.c
 * @brief Main application entry for Spresense 192kHz/24bit USB DAC
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int uac2_driver_register(void);
extern int uac2_audio_dma_init(void);

int main(int argc, char *argv[])
{
    printf("\n=======================================================\n");
    printf(" Spresense 192kHz / 24-bit USB Audio Class 2.0 (UAC2) DAC\n");
    printf(" Hardware: Sony CXD5602 + CXD5247 Audio Subsystem\n");
    printf(" Status: Phase 1 Descriptor & USB Driver Enumeration\n");
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

    printf("[UAC2] Driver registered! Connect Spresense USB port to PC.\n");
    printf("[UAC2] Ready for High-Speed (480Mbps) 192kHz/24bit streaming.\n");

    return 0;
}
