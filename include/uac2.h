/**
 * @file uac2.h
 * @brief USB Audio Class 2.0 (UAC2) Specification Constants and Types
 */

#ifndef __UAC2_H
#define __UAC2_H

#include <stdint.h>
#include <stdbool.h>

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
#define UAC2_BIT_DEPTH                24u
#define UAC2_SUBSLOT_SIZE             4u   /* 32-bit container */

/* High-Speed Microframe Calculation (125us = 8000 Hz) */
#define UAC2_HS_MICROFRAME_HZ         8000u
#define UAC2_SAMPLES_PER_UFRAME_192K  (UAC2_SAMPLE_RATE_192K / UAC2_HS_MICROFRAME_HZ) /* 24 */
#define UAC2_PACKET_SIZE_192K         (UAC2_SAMPLES_PER_UFRAME_192K * UAC2_CHANNELS * UAC2_SUBSLOT_SIZE) /* 192 bytes */

#endif /* __UAC2_H */
