# Spresense 192kHz / 24-bit Hi-Res USB DAC (UAC2)

Turn a Sony Spresense (CXD5602 + CXD5247) into a **USB Audio Class 2.0 (UAC2)
192kHz / 24-bit hi-res USB DAC**, with a from-scratch NuttX device driver
and firmware.

**Status: FW Final-1.0 verified working** (continuous YouTube playback and
192kHz/S32 `aplay` verified on Linux; frozen binary `nuttx.final-1.0.spk`).

---

## 1. Target specification

| Item | Spec | Notes |
|---|---|---|
| **USB** | USB 2.0 High-Speed (480 Mbps) | CXD5602 built-in USB PHY |
| **Audio class** | USB Audio Class 2.0 (UAC2) | Fully working on **Linux (ALSA), Raspberry Pi, Volumio, Android, etc.** |
| **Streaming layout** | **Single Alt 0 streaming** | Proprietary architecture conforming to CXD5602 silicon limits |
| **Sample rate** | **192.0 kHz** (hi-res, fixed) | RANGE advertises 192k-only; other values are clamped to 192k with ACK |
| **Bit depth** | **24-bit** (PCM) | 32-bit container (Subslot: 4 bytes) |
| **Channels** | 2ch (stereo L/R) | |
| **Sync mode** | **Adaptive** | Host-paced rate sync + jitter-absorbing ring buffer |
| **DAC / output** | Sony CXD5247 (DAC + S-Master amp) | 3.5 mm stereo jack |

---

## 2. 【Key】CXD5602 hardware (UDC IP) constraints

Close analysis of live USB traces (`usbmon`) and **CXD5602 User Manual
Section 3.18 "USB" (p.1118-1124)** proved decisive facts about the
controller IP (Synopsys DesignWare `DWC_d20ahb`).

### 2.1 Silicon-fixed parameters (User Manual p.1122-1123)
Synthesis-time configuration parameters burned into the silicon:

1. **Max alternate settings (p.1123 Table USB-46)**:
   - `Max Alternate Setting in Interfaces 0..14 Configuration 1` = **`1 (every case)`**
   - Each interface supports exactly **one alternate setting (Alt 0 only)**.
2. **Autonomous hardware STALL (p.1122 Table USB-45)**:
   - *"The UDC20-AHB Subsystem issues a STALL handshake for command interfaces [and settings] not supported in Configuration 1."*
   - A `SET_INTERFACE` for Alt > 0 is answered with an **autonomous STALL
     (-32 / EPIPE) in 137-250 us, with no interrupt raised to the CPU
     (NuttX / DCD) at all.**

### 2.2 Per-OS policy
- **Windows 10/11 (`usbaudio2.sys`): not possible with the stock driver**:
  - Per Microsoft's spec, an AS interface starts at zero-bandwidth Alt 0
    and must switch to Alt > 0 for streaming (Alt-0-only streaming is
    unsupported). On Spresense, Alt > 0 is STALLed by hardware, so the
    stock driver can never open a playback pin. Physically impossible.
- **Linux (ALSA / `snd-usb-audio`): fully supported (this project's focus)**:
  - Linux ALSA natively supports **single-Alt-0 streaming (isochronous EP
    placed directly on Alt 0, no zero-bandwidth Alt 0)**.
  - This project uses `UAC2_SINGLE_ALT0_STREAMING = 1` for a 192kHz/24-bit
    DAC on Linux / Raspberry Pi and similar transports.
- **Windows alternative**: a **WinUSB PoC (`tools/win_poc/`)** is in
  progress — MS OS 2.0 descriptors auto-bind WinUSB, and a user-mode
  transfer test streams with Alt 0 fixed. A formal kernel driver is on hold.

---

## 3. Bandwidth / packet math

- **Sample rate Fs**: 192,000 Hz
- **Bytes per frame**: 2 ch * 4 bytes (32-bit container) = 8 bytes
- **Total bit rate**: 192,000 * 8 * 8 = 12.288 Mbps
- **High-Speed microframe period**: 125 us (8,000/sec)
- **Samples per microframe**: 192,000 / 8,000 = 24 samples/uframe
- **Payload per microframe**: 24 * 8 = 192 bytes
- **wMaxPacketSize**: 200 bytes (jitter / clock-drift headroom included)

---

## 4. System architecture

```
+---------------------------------------------------------------+
|                       Host PC / Raspberry Pi                  |
|                 Linux ALSA (snd-usb-audio driver)             |
+---------------------------------------------------------------+
                               |  USB 2.0 High-Speed (480 Mbps)
                               |  SET_INTERFACE (Interface 1, Alt 0) -> ACK
                               v
+---------------------------------------------------------------+
|                 Sony Spresense (CXD5602 Main Core)            |
|                                                               |
|  [USB Controller (cxd56_usbdev.c)]                            |
|       |                                                       |
|       +--> EP0 Control (UAC2 AudioControl & ClockSource)      |
|       +--> EP2 OUT Isochronous (192kHz/24bit Audio Stream)    |
|       |                                                       |
|  [UAC2 Class Driver (uac2_driver.c)]                          |
|       |                                                       |
|  [Jitter-Absorbing Lock-Free Ring Buffer (128KB SPSC)]        |
|       |                                                       |
|  [Audio Subsystem / CXD5247 DMA Bridge (uac2_audio_dma.c)]    |
|       | (/dev/pcm0 - 192kHz / 24-in-32bit Slot)               |
+-------|-------------------------------------------------------+
        v
+---------------------------------------------------------------+
|                 Sony CXD5247 Audio PMIC / DAC                 |
|  - 24.576 / 49.152 MHz Master Clock                           |
|  - S-Master Digital Audio Amplifier / Modulator               |
|  - Integrated Low-Noise Headphone Amplifier                   |
+---------------------------------------------------------------+
        |
        v
    [3.5mm Headphone Jack Output]
```

Firmware internals (Final-1.0): always-feed pump (never starves the
engine), two-tier ring reserve, guarded retry-restart (never calls the
hanging STOP blindly), TRM-defined ERR tolerance, 192k-only RANGE with
clamp-ACK, ISR-safe prints (no `printf` in USB interrupt context), USB IRQ
demoted below audio, and a clock-drift servo (single-frame drop/repeat,
inaudible) that bounds the ring level forever.

---

## 5. Development phases (roadmap)

### Phase 1: USB enumeration and UAC2 descriptors【Done】
- NuttX `usbdevclass_driver_s` skeleton
- UAC2 descriptor tree (IAD, AC, AS, ClockSource, Terminals)
- Full PCM device recognition in Linux ALSA (`snd-usb-audio`), 192kHz/24-bit

### Phase 2: Silicon-constraint analysis and HW verification【Done】
- Autonomous STALL on Alt > 0 proven with raw `usbmon` logs (137-250 us)
- "Alt 0 only" silicon spec proven via CXD5602 User Manual (Table USB-45/46)
- `UAC2_SINGLE_ALT0_STREAMING = 1` Alt-0-only streaming layout fixed

### Phase 3: Isochronous streaming receive + ring buffer feed【Done】
- EP2 OUT (Adaptive Isochronous) allocation and packet receive callbacks
- Continuous 192 bytes/125us isochronous receive into a lock-free ring buffer

### Phase 4: Audio subsystem (CXD5247 / S-Master) integration【Done】
- CXD5247 S-Master DAC (`/dev/pcm0`) at 192kHz / 24-bit
- Real audio data pipeline from ring buffer to audio DMA
- Headphone output verified with `aplay` (1kHz hi-res source) on a Linux host
- Declared complete as Final-1.0

### Phase 5: Windows transfer PoC【In progress】
- MS OS 2.0 descriptors (BOS + MI_01 -> WINUSB) for INF-free auto bind
- `tools/win_poc/winusb_poc.c` (plain Windows SDK + MSVC, no WDK) for Alt 0
  transfer tests

---

## 6. Directory layout

- `include/`: UAC2 spec defines, descriptor structs, ring buffer
- `src/`: UAC2 device driver, descriptor tables, DMA bridge, app
- `docs/`: tech notes, clock-sync theory, reboot-resume log (Japanese)
- `tools/`: verification / tone-generation scripts, log tools, `win_poc/` (Windows transfer PoC)
- `test_logs/`: raw logs (`usbmon`, serial) proving the HW autonomous STALL
- `nuttx.final-1.0.spk`: verified frozen binary (flash as-is)
- `spresense_192k24b_1khz.wav`: 1kHz test tone (192kHz/S32/stereo)

---

## 7. Build

Prerequisites: Spresense SDK (`nuttx/` + `sdk/`), ARM GCC (`spresense-tools`),
Ubuntu/WSL.

```bash
# Only needed if different from the defaults ($HOME/spresense, $HOME/spresense-tools)
export SPRESENSE=/path/to/spresense
export SPRESENSE_TOOLS=/path/to/spresense-tools
export UAC2_TEST_HOST="user@linux-host"   # for tools/run_*.sh remote tests

./build_and_flash.sh COM6   # e.g. /dev/ttyUSB0 on Linux
```

`nuttx.spk` is generated under `sdk/` and flashed by the same script.

---

## 8. License

Apache License 2.0 (see `LICENSE`). SDK modifications made for this project
are distributed as documented source procedures; the SDK tree itself is not
redistributed here.
