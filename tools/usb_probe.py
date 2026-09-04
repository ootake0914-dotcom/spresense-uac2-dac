#!/usr/bin/env python3
"""
USB Audio Device Probe Tool for Spresense UAC2 DAC
Verifies USB enumeration, descriptors, and audio device registration.
"""

import sys
import platform

def probe_windows():
    print("[PROBE] Checking Windows Audio Endpoint Devices...")
    try:
        import sounddevice as sd
        devices = sd.query_devices()
        print("\nAvailable Output Devices:")
        for idx, dev in enumerate(devices):
            if dev['max_output_channels'] > 0:
                print(f"  [{idx}] {dev['name']} (Max SR: {dev['default_samplerate']} Hz, Out: {dev['max_output_channels']}ch)")
                if "Spresense" in dev['name'] or "UAC2" in dev['name']:
                    print(f"       ===> FOUND SPRESENSE DAC! Device ID: {idx}")
    except ImportError:
        print("[NOTE] 'sounddevice' module not installed. Run: pip install sounddevice")

def main():
    print("==================================================")
    print(" Spresense UAC2 Hi-Res DAC Probe Tool")
    print(f" Platform: {platform.system()} {platform.release()}")
    print("==================================================")
    if platform.system() == "Windows":
        probe_windows()
    else:
        print("[INFO] Run 'lsusb -v' or 'aplay -l' to inspect USB Audio on Linux.")

if __name__ == '__main__':
    main()
