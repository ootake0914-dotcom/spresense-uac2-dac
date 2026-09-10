# Spresense 192 kHz / 24-bit Hi-Res USB-DAC (UAC2)

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![RTOS](https://img.shields.io/badge/RTOS-NuttX-green.svg)](https://nuttx.apache.org/)
[![USB](https://img.shields.io/badge/USB-Audio_Class_2.0-orange.svg)](https://www.usb.org/)
[![PCM](https://img.shields.io/badge/PCM-192kHz_24--bit-purple.svg)]()
[![Audio](https://img.shields.io/badge/Audio-Bit--Perfect-gold.svg)]()

Turn a **Sony Spresense** (CXD5602 + CXD5247) into a **dedicated 192 kHz / 24-bit USB Audio Class 2.0 (UAC2) hi-res USB-DAC** with a custom NuttX device driver and firmware.

> [!CAUTION]
> **Non-standard descriptor design (deliberate):** this firmware streams on **Alt-0 with endpoints populated** (`bAlternateSetting: 0`, `bNumEndpoints: 2`). The UAC2 spec requires Alt-0 to be zero-bandwidth, with streaming starting at Alt-1+. This was chosen because the CXD5602 USB device controller autonomously STALLs any `SET_INTERFACE` to Alt > 0 (hardware value-gating, confirmed by experiment), making spec-compliant Alt-1/Alt-2 streaming impossible on this silicon. Linux `snd-usb-audio` accepts Alt-0 streaming; Windows (`usbaudio2.sys`) and macOS may refuse to create an audio pin. **Linux / Raspberry Pi / Volumio only.**

---

## Features

- **192 kHz / 24-bit Native S-Master Output** — Direct audio rendering via Sony CXD5247 S-Master PWM full-digital amplifier at 192 kHz native clock.
- **Direct MSB-Aligned Passthrough** — Host 24-in-32-bit PCM audio stream is fed directly into the audio DMA engine without software scaling, bit-shift truncation, or digital filtering. Bit-perfect (`raw == dst`, CRC-matched) **only under steady-state conditions: servo non-intervention (`svd/svu/dup/gap == 0`), volume 0 dB, mute off**; drop servo drops frames, dup servo repeats frames, and any volume/mute setting intentionally alters the output.
- **Zero-Copy Direct-to-DMA Pipeline** — Eliminates intermediate staging buffers; the SPSC ring buffer feeds NuttX APB DMA audio buffers directly.
- **Adaptive Clock-Drift Compensation** — Built-in drift compensation servo maintains a healthy ring buffer cushion (~64–70 KB) to absorb physical crystal offset indefinitely.
- **8-Byte Stereo Frame Protection** — Enforces strict 8-byte frame boundary alignment under all conditions, preventing stereo phase issues.
- **UAC2 Asynchronous Descriptors** — Standard UAC2 Async descriptors (`0x05` + `bSynchAddress = 0x81`) with real-time feedback telemetry.
- **Linux / Raspberry Pi / Volumio Ready** — Single-Alt-0 streaming design natively supported by Linux ALSA (`snd-usb-audio`).

---

## System Architecture

```mermaid
flowchart LR
    PC["Linux / Raspberry Pi / Volumio<br/>ALSA snd-usb-audio"] -- "USB 2.0 High-Speed (480 Mbps)<br/>EP2 OUT Isochronous<br/>192 B / 125 us" --> UDC["CXD5602 UDC<br/>USB Device Controller"]
    UDC -- "Lock-free ISR push" --> RING["128 KB SPSC Ring Buffer<br/>8-byte frame guard"]
    RING -- "Zero-copy pump thread<br/>Clock-drift compensation" --> APB["NuttX Audio DMA Engine<br/>16x APB buffers (/dev/pcm0)"]
    APB --> SM["Sony CXD5247 S-Master<br/>49.152 MHz Master Clock"]
    SM --> HP["3.5 mm Stereo Output"]
```

---

## Hardware Requirements

- **Sony Spresense Main Board** + **Extension Board**
- Micro-USB cable connected to the **Extension Board USB port** (Audio streaming)
- USB-UART connection for console/diagnostic logs (`115200 bps`, e.g. `COMx` on Windows or `/dev/ttyUSBx` on Linux)
- Audio Host: Linux PC, Raspberry Pi, Volumio, Android, etc.

---

## Quick Start

### Option 1: Flash Pre-built Firmware (Recommended - No SDK Build Required)

If you simply want to use Spresense as a Hi-Res USB-DAC immediately:

1. Download `nuttx-uac2-Rev84.spk` from [GitHub Releases](https://github.com/ootake0914-dotcom/spresense-uac2-dac/releases).
2. Flash it using the official Spresense flashing tool (`flash_writer.py` from Spresense SDK or `spresense-tools`):

```bash
# Windows (PowerShell / Command Prompt)
python flash_writer.py -c COM6 -b 115200 -d -n nuttx-uac2-Rev84.spk

# Linux / macOS
python flash_writer.py -c /dev/ttyUSB0 -b 115200 -d -n nuttx-uac2-Rev84.spk
```

### Option 2: Build from Source

#### Prerequisites
- Sony Spresense SDK (`nuttx/` + `sdk/`)
- GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`)
- Ubuntu / Debian / WSL environment

#### Build and Flash

```bash
# Optional: Set environment paths if custom
export SPRESENSE=/path/to/spresense
export SPRESENSE_TOOLS=/path/to/spresense-tools

# Build and flash to target board via serial port
./build_and_flash.sh <serial-port>
# Example: ./build_and_flash.sh COM6
# Example: ./build_and_flash.sh /dev/ttyUSB0
```

### Live Status Monitoring

To monitor real-time playback, bit-perfect CRC stats, and buffer telemetry:
```bash
python tools/record_serial.py 10 test_logs/spresense_serial.log
```

---

## Specifications

| Parameter | Specification | Notes |
| :--- | :--- | :--- |
| **USB Class** | USB Audio Class 2.0 (High-Speed 480 Mbps) | CXD5602 integrated USB PHY |
| **Fidelity** | **Direct passthrough (conditionally bit-perfect)** | 24-bit MSB-aligned, no DSP; bit-perfect only when `svd/svu/dup/gap == 0`, 0 dB, mute off (live CRC32 audit) |
| **Sampling Rate** | 192.0 kHz (Fixed) | High-resolution audio |
| **Bit Depth** | 24-bit PCM | In 32-bit container (Subslot: 4 bytes) |
| **Channels** | 2 channels (Stereo) | Front Left / Front Right |
| **Microframe Interval** | 125 µs (8,000 packets/sec) | 24 samples (192 bytes) / packet |
| **Max Packet Size** | 200 bytes | Includes drift / jitter headroom |
| **DAC / Amp** | Sony CXD5247 (S-Master PWM) | 3.5 mm headphone jack |

---

## Limitations & Host Compatibility

- **Linux (ALSA)**: Fully supported out of the box. Compatible with desktop Linux, Raspberry Pi, Volumio, and Android.
- **Windows (Stock Driver)**: Windows stock `usbaudio2.sys` requires an alternate setting switch (Alt > 0) to start playback. Because the CXD5602 USB hardware autonomously STALLs Alt > 0 requests (silicon-level constraint), the stock driver cannot open audio endpoints directly. A user-mode WinUSB streaming PoC is available under `tools/win_poc/`.
- **Clock Drift Handling**: Async descriptors advertised (`0x05` + EP1 feedback). Feedback EP is configured and paced-submit is exercised (Rev85: submit-failure guard + double-buffered payload), but IN completions never arrive on this silicon (DMA descriptor stays pristine; host ignores the 4 zero bytes and holds nominal pacing). Effective drift absorption is the device-side drop/dup servo + 128 KiB ring — i.e. open-loop with live PI telemetry, not closed-loop.

---

## Project Structure

- `include/` — UAC2 specification headers, descriptor structs, ring buffer definitions
- `src/` — UAC2 driver implementation, USB descriptors, audio DMA bridge, main application
- `docs/` — Technical notes and design documentation
- `tools/` — Diagnostics, serial monitoring scripts, test tone generators, WinUSB PoC
- `test_logs/` — Serial and USB protocol verification logs

---

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.
