#!/usr/bin/env python3
"""
USB Audio Device Probe Tool for Spresense UAC2 DAC (Phase 1: Enumeration)
Verifies USB enumeration, descriptors, and audio device registration.

Expected device:
  VID = 054C (Sony), PID = 0CEC (current; Rev 7: PID 0x0CEC, Adaptive Mode)
  Strings: Sony / Spresense 192kHz/24bit Audio
  Config : 192kHz, 2ch, 24-bit (32-bit slot), 200 bytes/uframe
"""
import sys
import platform
import subprocess

VID = "054c"
EXPECT_SR = 192000


def probe_windows():
    print("[PROBE] Checking Windows Audio Endpoint Devices...")
    found = False
    try:
        import sounddevice as sd
        devices = sd.query_devices()
        print("\nAvailable Output Devices:")
        for idx, dev in enumerate(devices):
            if dev['max_output_channels'] > 0:
                mark = ""
                if ("Spresense" in dev['name'] or "UAC2" in dev['name']
                        or "192kHz" in dev['name']):
                    mark = "  ===> FOUND SPRESENSE DAC!"
                    found = True
                print(f"  [{idx}] {dev['name']} "
                      f"(SR: {dev['default_samplerate']} Hz, "
                      f"Out: {dev['max_output_channels']}ch){mark}")
                if mark and dev['default_samplerate'] != EXPECT_SR:
                    print(f"       [WARN] Expected default SR {EXPECT_SR}, "
                          f"got {dev['default_samplerate']} "
                          f"(check Sound panel -> 192kHz/24bit).")
    except ImportError:
        print("[NOTE] 'sounddevice' not installed. Run: pip install sounddevice")
        print("[HINT] Or check: Settings -> System -> Sound -> Output devices,")
        print("       look for 'Spresense 192kHz/24bit Audio'.")
    print("\n[HINT] Device Manager check:")
    print("  - OK   : 'Sound, video and game controllers' -> USB Audio 2.0 device, no yellow bang.")
    print("  - FAIL : 'Unknown USB Device (Device Descriptor Request Failed)'")
    print("           -> descriptor/GET_DESCRIPTOR bug, re-check firmware log.")
    return found


def probe_linux():
    ok = False
    print("[PROBE] Linux USB Audio check (VID 054c, any PID)...")
    for cmd in (["lsusb"], ["lsusb", "-v", "-d", "054c:"], ["aplay", "-l"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            print(f"\n$ {' '.join(cmd)}")
            print(out.stdout[:4000] or "(no output)")
            if out.stdout and ("054c" in out.stdout.lower()
                               or "spresense" in out.stdout.lower()
                               or "uac2" in out.stdout.lower()):
                ok = True
        except FileNotFoundError:
            print(f"  [SKIP] {cmd[0]} not installed.")
        except Exception as e:  # noqa: BLE001
            print(f"  [ERR] {' '.join(cmd)}: {e}")
    if not ok:
        print("\n[FAIL] Spresense VID:PID not found. Check dmesg:")
        print("  dmesg | tail -n 30   (look for 'new high-speed USB device', 'USB Audio Class 2.0')")
    else:
        print("\n[PASS] USB descriptor visible. Next: check 'cat /proc/asound/cards'.")
    return ok


def main():
    print("==================================================")
    print(" Spresense UAC2 Hi-Res DAC Probe Tool (Phase 1)")
    print(f" Platform: {platform.system()} {platform.release()}")
    print(f" Expect: VID {VID} (PID 0CEA current), {EXPECT_SR} Hz, 2ch, 24-bit")
    print("==================================================")
    plat = platform.system()
    if plat == "Windows":
        ok = probe_windows()
    elif plat == "Linux":
        ok = probe_linux()
    else:
        print(f"[INFO] Unsupported platform {plat}.")
        print("  macOS: check 'System Settings -> Sound -> Output' + 'Audio MIDI Setup' (192kHz).")
        ok = False
    print("\n[PHASE 1 PASS CRITERIA]")
    print("  1. Device appears without yellow-bang / descriptor error.")
    print("  2. Name contains 'Spresense' or 'USB Audio'.")
    print("  3. Supported format includes 192000 Hz, 2ch, 24-bit.")
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
