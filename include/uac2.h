/**
 * @file uac2.h
 * @brief USB Audio Class 2.0 (UAC2) Specification Constants and Types
 */

#ifndef __UAC2_H
#define __UAC2_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <arch/irq.h>

/* ISR-safe print (Rev66): task context only.
 * USB completion (ISO) and EP0 setup run in USB interrupt context,
 * where console output wedges/corrupts the port (B1). up_interrupt_context()
 * is a static inline in arch/irq.h (no call cost issue).
 */
#define UAC2_TPRINTF(...) do { if (!up_interrupt_context()) { printf(__VA_ARGS__); fflush(stdout); } } while (0)

/* TEMP-DIAG: periodic-print silence switch.
 * Rev65-eyes: temporarily 0 for servo verification (short run only).
 */
#define UAC2_SILENT_DIAG 0

/* Audio Function Subclass Codes */
#define UAC2_SUBCLASS_UNDEFINED       0x00
#define UAC2_SUBCLASS_AUDIOCONTROL    0x01
#define UAC2_SUBCLASS_AUDIOSTREAMING  0x02
#define UAC2_SUBCLASS_MIDISTREAMING   0x03

/* Audio Class-Specific AC Interface Descriptor Subtypes */
#define UAC2_AC_HEADER                0x01
#define UAC2_AC_INPUT_TERMINAL        0x02
#define UAC2_AC_OUTPUT_TERMINAL       0x03
#define UAC2_AC_MIXER_UNIT            0x04
#define UAC2_AC_SELECTOR_UNIT         0x05
#define UAC2_AC_FEATURE_UNIT          0x06
#define UAC2_AC_EFFECT_UNIT           0x07
#define UAC2_AC_PROCESSING_UNIT       0x08
#define UAC2_AC_EXTENSION_UNIT        0x09
#define UAC2_AC_CLOCK_SOURCE          0x0A
#define UAC2_AC_CLOCK_SELECTOR        0x0B
#define UAC2_AC_CLOCK_MULTIPLIER      0x0C
#define UAC2_AC_SAMPLE_RATE_CONV      0x0D

/* Audio Class-Specific AS Interface Descriptor Subtypes */
#define UAC2_AS_GENERAL               0x01
#define UAC2_AS_FORMAT_TYPE           0x02
#define UAC2_AS_ENCODER               0x03
#define UAC2_AS_DECODER               0x04

/* Audio Class-Specific Endpoint Descriptor Subtypes */
#define UAC2_EP_GENERAL               0x01

/* Terminal Types */
#define UAC2_TERMINAL_UNDEFINED       0x0100
#define UAC2_TERMINAL_STREAMING       0x0101
#define UAC2_OUTPUT_TERMINAL_SPEAKER  0x0301
#define UAC2_OUTPUT_TERMINAL_HEADPHONES 0x0302

/* Clock Source Attributes */
#define UAC2_CLOCK_SOURCE_EXTERNAL    0x00
#define UAC2_CLOCK_SOURCE_INT_FIXED   0x01
#define UAC2_CLOCK_SOURCE_INT_VAR     0x02
#define UAC2_CLOCK_SOURCE_INT_PROG    0x03

/* Audio Class 2.0 Request Codes */
#define UAC2_CS_CUR                   0x01
#define UAC2_CS_RANGE                 0x02
#define UAC2_CS_MEM                   0x03

/* Clock Source Control Selectors */
#define UAC2_CS_CONTROL_UNDEFINED     0x00
#define UAC2_CS_CONTROL_SAM_FREQ      0x01
#define UAC2_CS_CONTROL_CLOCK_VALID   0x02

/* Target Audio Parameters */
#define UAC2_SAMPLE_RATE_192K         192000u
#define UAC2_SAMPLE_RATE_96K          96000u
#define UAC2_SAMPLE_RATE_48K          48000u
#define UAC2_SAMPLE_RATE_44K1         44100u

#define UAC2_CHANNELS                 2u

/* Format 1: 16-bit PCM (Alt Setting 1) */
#define UAC2_BIT_DEPTH_16             16u
#define UAC2_SUBSLOT_SIZE_16          2u   /* 16-bit container */

/* Format 2: 24-bit PCM in 32-bit slot (Alt Setting 2) */
#define UAC2_BIT_DEPTH_24             24u
#define UAC2_SUBSLOT_SIZE_24          4u   /* 32-bit container */

/* Legacy aliases */
#define UAC2_BIT_DEPTH                UAC2_BIT_DEPTH_24
#define UAC2_SUBSLOT_SIZE             UAC2_SUBSLOT_SIZE_24

/* High-Speed Microframe Calculation (125us = 8000 Hz)
 * In Asynchronous/Adaptive mode (UAC2 FMT-2.0 2.3.1.1), wMaxPacketSize must include
 * +1 audio slot (sample) to accommodate host-side clock drift adjustment.
 * Nominally 192000 / 8000 = 24 samples. Max packet = 24 + 1 = 25 samples.
 * Nominally 48000 / 8000 = 6 samples. Max packet = 6 + 1 = 7 samples.
 */
#define UAC2_HS_MICROFRAME_HZ         8000u

#define UAC2_NOMINAL_SAMPLES_48K      (UAC2_SAMPLE_RATE_48K / UAC2_HS_MICROFRAME_HZ)   /* 6 */
#define UAC2_MAX_SAMPLES_48K          (UAC2_NOMINAL_SAMPLES_48K + 1u)                 /* 7 */
#define UAC2_PACKET_SIZE_24BIT_48K    (UAC2_MAX_SAMPLES_48K * UAC2_CHANNELS * UAC2_SUBSLOT_SIZE_24) /* 56 bytes */

#define UAC2_NOMINAL_SAMPLES_192K     (UAC2_SAMPLE_RATE_192K / UAC2_HS_MICROFRAME_HZ) /* 24 */
#define UAC2_MAX_SAMPLES_192K         (UAC2_NOMINAL_SAMPLES_192K + 1u)               /* 25 */

#define UAC2_PACKET_SIZE_16BIT_192K   (UAC2_MAX_SAMPLES_192K * UAC2_CHANNELS * UAC2_SUBSLOT_SIZE_16) /* 100 bytes */
#define UAC2_PACKET_SIZE_24BIT_192K   (UAC2_MAX_SAMPLES_192K * UAC2_CHANNELS * UAC2_SUBSLOT_SIZE_24) /* 200 bytes */
#define UAC2_PACKET_SIZE_192K         UAC2_PACKET_SIZE_24BIT_192K


#endif /* __UAC2_H */
