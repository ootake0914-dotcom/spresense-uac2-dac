#!/usr/bin/env python3
"""
Spresense UAC2 DAC: WASAPI Exclusive-mode 192kHz/24bit sound test.

- Generates a 1 kHz sine reference WAV (192 kHz, stereo, 24-bit PCM).
- Plays it through the Spresense device in WASAPI Exclusive mode,
  bypassing the Windows Audio Engine (works even while Shared Mode /
  GetMixFormat is still broken - issue #1).

Usage (Windows):
    pip install sounddevice
    python tools\\wasapi_exclusive_test.py [--seconds 5] [--freq 1000]
                                           [--device <id|name>] [--no-play]

On-device check (115200 bps NSH console, running `uac2_dac`):
    [UAC2 #n] STREAMING (192kHz Active) | Alt:1 | SR:192000 Hz | Buf:<growing> B ...
Headphone jack should output a 1 kHz tone.
"""

import argparse
import math
import platform
import struct
import sys
import wave

SR = 192000
CH = 2
FREQ_DEFAULT = 1000.0
SECONDS_DEFAULT = 5
WAV_NAME = "spresense_192k24b_1khz.wav"


def gen_sine_float(freq, seconds, amplitude=0.5):
    """Mono float samples (-1.0..1.0). 192000/1000 = 192 samples/cycle exact."""
    n = int(SR * seconds)
    step = 2.0 * math.pi * freq / SR
    return [amplitude * math.sin(step * i) for i in range(n)]


def write_wav24(path, mono, amplitude_scale=1.0):
    """Write stereo 24-bit PCM WAV (2ch duplicated mono)."""
    peak = int(8388607 * amplitude_scale)
    with wave.open(path, "wb") as w:
        w.setnchannels(CH)
        w.setsampwidth(3)
        w.setframerate(SR)
        frames = bytearray()
        for s in mono:
            v = max(-8388608, min(8388607, int(s * 8388607)))
            if v < 0:
                v += 1 << 24
            b = struct.pack("<i", v)[:3]  # little-endian, low 3 bytes
            frames += b + b  # L + R
        w.writeframes(bytes(frames))
    print(f"[WAV] wrote {path} ({len(mono)/SR:.1f}s, 24-bit, {SR} Hz stereo)")


def find_device(hint=None):
    import sounddevice as sd
    devs = sd.query_devices()
    if hint is not None:
        try:
            idx = int(hint)
            print(f"[DEV] using device id {idx}: {devs[idx]['name']}")
            return idx
        except (ValueError, IndexError):
            pass
        for i, d in enumerate(devs):
            if hint.lower() in d["name"].lower() and d["max_output_channels"] > 0:
                print(f"[DEV] matched '{hint}' -> [{i}] {d['name']}")
                return i
        print(f"[ERR] no output device matches '{hint}'")
    print("[DEV] output devices:")
    found = None
    for i, d in enumerate(devs):
        if d["max_output_channels"] > 0:
            mark = ""
            if any(k in d["name"] for k in ("Spresense", "UAC2", "192kHz", "USB Audio")):
                mark = "   <== candidate"
                if found is None:
                    found = i
            print(f"  [{i}] {d['name']} (default SR {d['default_samplerate']}){mark}")
    if found is None:
        print("[ERR] no Spresense-like device. Re-run with --device <id>.")
        sys.exit(2)
    print(f"[DEV] auto-selected [{found}]")
    return found


def play_exclusive(mono, device):
    import sounddevice as sd
    stereo = [[s, s] for s in mono]  # sounddevice wants (frames, channels)
    try:
        import numpy as np
        data = np.array(stereo, dtype="float32")
    except ImportError:
        data = stereo  # PortAudio accepts nested lists (float)
    print(f"[PLAY] WASAPI exclusive, {SR} Hz, {len(mono)/SR:.1f}s ...")
    print("        (check NSH console: state should turn STREAMING, Buf growing)")
    sd.play(data, SR, device=device,
            extra_settings=sd.WasapiSettings(exclusive=True))
    sd.wait()
    print("[PLAY] done. Did the headphone output a 1 kHz tone? (Y => Phase 3 path OK)")


def main():
    ap = argparse.ArgumentParser(description="Spresense UAC2 exclusive-mode sound test")
    ap.add_argument("--seconds", type=float, default=SECONDS_DEFAULT)
    ap.add_argument("--freq", type=float, default=FREQ_DEFAULT)
    ap.add_argument("--device", default=None, help="device id or name substring")
    ap.add_argument("--no-play", action="store_true", help="only generate WAV")
    args = ap.parse_args()

    print("==================================================")
    print(f" Spresense UAC2 exclusive test: {args.freq} Hz sine, "
          f"{SR} Hz/24-bit, {args.seconds}s")
    print("==================================================")

    mono = gen_sine_float(args.freq, args.seconds)
    write_wav24(WAV_NAME, mono)

    if args.no_play:
        print(f"[INFO] WAV only. Play it in exclusive mode from any player, or "
              f"re-run without --no-play.")
        return 0
    if platform.system() != "Windows":
        print("[INFO] non-Windows: play the WAV in exclusive/bit-perfect mode "
              "manually.")
        return 0
    try:
        import sounddevice  # noqa: F401
    except ImportError:
        print("[ERR] 'sounddevice' not installed. Run: pip install sounddevice")
        print(f"      WAV file '{WAV_NAME}' was still generated.")
        return 1
    dev = find_device(args.device)
    try:
        play_exclusive(mono, dev)
    except Exception as e:  # noqa: BLE001
        print(f"[ERR] exclusive playback failed: {e}")
        print("      Hints: close apps using the device; check Device Manager "
              "has no yellow-bang; try --device <id>.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
