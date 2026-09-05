#!/usr/bin/env python3
"""
Fix Windows Shared Mode for the Spresense UAC2 DAC (issue #1).

Symptom : IAudioClient::GetMixFormat -> AUDCLNT_E_UNSUPPORTED_FORMAT
          (0x88890008), volume slider stuck at 0%, no "Details" tab in
          mmsys.cpl (no PKEY_AudioEngine_DeviceFormat in registry).
Cause   : AudioSrv never wrote the default mix format for this endpoint
          (first detection happened while DataRangeIntersection could not
          complete). Re-enumeration alone does not repair the stale state.
Fix     : bind a default format once via the endpoint's property store
          (PKEY_AudioEngine_DeviceFormat), then restart AudioSrv.
Note    : the registry value is a serialized PROPVARIANT: an 8-byte header
          (41 00 00 00 01 00 00 00, i.e. VT_BLOB) followed by the 40-byte
          WAVEFORMATEXTENSIBLE -> 48 bytes total. A bare 40-byte WFEX is
          misparsed (vt reads 0xFFFE) and AudioSrv keeps short-circuiting
          with 0x88890008, so the header is mandatory.

Usage (Windows, Administrator CMD / PowerShell):
    python tools\\fix_shared_mode.py                # dry-run: list endpoints
    python tools\\fix_shared_mode.py --apply        # write 192kHz default
    python tools\\fix_shared_mode.py --apply --rate 48000

After --apply: reboot (recommended) or `net stop Audiosrv` + `net start Audiosrv`,
then re-run tools\\usb_probe.py and check the "Details" tab / volume slider.
To revert: Sound panel -> device Properties -> Advanced -> pick another
default format (Windows rewrites the key), or uninstall the device in
Device Manager (keeps firmware untouched).
"""

import argparse
import struct
import sys

RENDER_KEY = (r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices"
              r"\Audio\Render")
PKEY_DEVFORM = "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0"
PKEY_OEMFORM = "{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},0"
PKEY_PERIODICITY = "{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},1"
# String-valued keys used for display / matching (verified on Win11:
# endpoint name, device description, interface path w/ VID/PID, USB id).
# NOTE: PKEY_Device_FriendlyName (...,14) is typically absent on audio
# endpoints, so do NOT rely on it.
PKEY_STRINGS = (
    "{a45c254e-df1c-4efd-8020-67d146a850e0},2",   # endpoint name
    "{b3f8fa53-0004-438e-9003-51a46e139bfc},6",   # device description
    "{b3f8fa53-0004-438e-9003-51a46e139bfc},2",   # interface path (VID/PID)
    "{80f111c3-b103-42e1-afb6-db7a6fa8be1f},0",   # USB id (VID/PID)
)
DEVICE_STATE = {1: "ACTIVE", 2: "DISABLED", 4: "NOTPRESENT", 8: "UNPLUGGED"}
# KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71}
SUBTYPE_PCM = bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                     0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
# KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
SUBTYPE_IEEE_FLOAT = bytes([0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])

RATES = (44100, 48000, 88200, 96000, 176400, 192000)


def wave_format_extensible_24in32(rate):
    """40-byte WAVEFORMATEXTENSIBLE: stereo, 32-bit container, 24 valid bits (PCM)."""
    ch, bits, valid, mask = 2, 32, 24, 0x00000003
    block = ch * (bits // 8)
    avg = rate * block
    header = struct.pack("<HHIIHHH", 0xFFFE, ch, rate, avg, block, bits, 22)
    ext = struct.pack("<HI", valid, mask) + SUBTYPE_PCM
    blob = header + ext
    assert len(blob) == 40, len(blob)
    return blob


def wave_format_extensible_float32(rate):
    """40-byte WAVEFORMATEXTENSIBLE: stereo, 32-bit float (AudioEngine shared mix format)."""
    ch, bits, valid, mask = 2, 32, 32, 0x00000003
    block = ch * (bits // 8)
    avg = rate * block
    header = struct.pack("<HHIIHHH", 0xFFFE, ch, rate, avg, block, bits, 22)
    ext = struct.pack("<HI", valid, mask) + SUBTYPE_IEEE_FLOAT
    blob = header + ext
    assert len(blob) == 40, len(blob)
    return blob


# Serialized-PROPVARIANT header for a VT_BLOB registry value, as observed on
# working endpoints' PKEY_AudioEngine_DeviceFormat (8 bytes).
PROPVARIANT_BLOB_HEADER = bytes([0x41, 0x00, 0x00, 0x00,
                                 0x01, 0x00, 0x00, 0x00])


def pkey_device_format_blob(rate):
    """48-byte registry value: PROPVARIANT header + WAVEFORMATEXTENSIBLE (PCM 24in32)."""
    blob = PROPVARIANT_BLOB_HEADER + wave_format_extensible_24in32(rate)
    assert len(blob) == 48, len(blob)
    return blob


def pkey_oem_format_blob(rate):
    """48-byte registry value: PROPVARIANT header + WAVEFORMATEXTENSIBLE (Float32 mix format)."""
    blob = PROPVARIANT_BLOB_HEADER + wave_format_extensible_float32(rate)
    assert len(blob) == 48, len(blob)
    return blob


def is_admin():
    try:
        import ctypes
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except Exception:  # noqa: BLE001
        return False


def list_render_endpoints():
    import winreg
    out = []
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, RENDER_KEY) as render:
        guids, i = [], 0
        while True:
            try:
                guids.append(winreg.EnumKey(render, i))
            except OSError:
                break
            i += 1
        for guid in guids:
            try:
                with winreg.OpenKey(render, guid) as ep:
                    try:
                        state, _ = winreg.QueryValueEx(ep, "DeviceState")
                    except OSError:
                        state = 0
                with winreg.OpenKey(render, guid + r"\Properties") as props:
                    strs = {}
                    for key in PKEY_STRINGS:
                        try:
                            val, _ = winreg.QueryValueEx(props, key)
                            strs[key] = str(val)
                        except OSError:
                            pass
                    name = (strs.get(PKEY_STRINGS[0])
                            or strs.get(PKEY_STRINGS[1])
                            or "(no name)")
                    try:
                        cur_dev, _ = winreg.QueryValueEx(props, PKEY_DEVFORM)
                        has_dev = f"{len(cur_dev)}B"
                    except OSError:
                        has_dev = "MISSING"
                    try:
                        cur_oem, _ = winreg.QueryValueEx(props, PKEY_OEMFORM)
                        has_oem = f"{len(cur_oem)}B"
                    except OSError:
                        has_oem = "MISSING"
                    has_fmt = f"DevForm:{has_dev}, OEMForm:{has_oem}"
                    if has_dev != "48B" or has_oem != "48B":
                        has_fmt += "  <-- shared mode broken"
                    out.append((guid, name, state, has_fmt, strs))
            except OSError:
                continue
    return out


PERIODICITY_10MS_BLOB = (PROPVARIANT_BLOB_HEADER +
                          struct.pack("<Q", 100000))  # 10ms in 100ns units


def apply_default_format(guid, dev_blob, oem_blob):
    import winreg
    path = RENDER_KEY + "\\" + guid + r"\Properties"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0,
                            winreg.KEY_SET_VALUE) as props:
            # 1. DeviceFormat (Hardware PCM format for KS pin: 24-bit in 32-bit slot)
            winreg.SetValueEx(props, PKEY_DEVFORM, 0, winreg.REG_BINARY, dev_blob)
            # 2. OEMFormat (AudioEngine shared mix format: IEEE Float 32-bit)
            winreg.SetValueEx(props, PKEY_OEMFORM, 0, winreg.REG_BINARY, oem_blob)
            # 3. OEMPeriodicity (Engine buffering interval: 10ms)
            winreg.SetValueEx(props, PKEY_PERIODICITY, 0, winreg.REG_BINARY,
                              PERIODICITY_10MS_BLOB)
    except PermissionError:
        print("[ERR] access denied writing HKLM. Re-run as Administrator.")
        raise SystemExit(1)
    print(f"[OK] wrote DeviceFormat (PCM 24b) + OEMFormat (Float 32b) + Periodicity to ...\\{guid}")


def main():
    ap = argparse.ArgumentParser(description="Bind a default mix format (shared-mode fix)")
    ap.add_argument("--apply", action="store_true", help="actually write registry (default: dry-run)")
    ap.add_argument("--rate", type=int, default=192000, choices=RATES)
    ap.add_argument("--match", default="spresense",
                    help="substring matched against endpoint name / device "
                         "description / VID_PID strings (case-insensitive)")
    ap.add_argument("--guid", default=None,
                    help="target endpoint GUID directly (skips matching)")
    ap.add_argument("--any-state", action="store_true",
                    help="allow targeting non-ACTIVE (ghost) endpoints")
    args = ap.parse_args()

    if sys.platform != "win32":
        print("[ERR] this script is Windows-only.")
        return 1

    eps = list_render_endpoints()
    print("Render endpoints:")
    for guid, name, state, fmt, _ in eps:
        print(f"  {guid}  [{DEVICE_STATE.get(state, state)}] '{name}'  "
              f"DeviceFormat: {fmt}")

    if args.guid:
        cand = [e for e in eps if e[0].lower() == args.guid.lower()]
        if not cand:
            print(f"\n[ERR] no endpoint with GUID {args.guid}.")
            return 2
        guid, name, state, fmt, _ = cand[0]
    else:
        def matches(e):
            _, _, _, _, strs = e
            hay = "\n".join(strs.values()).lower()
            return args.match.lower() in hay
        cand = [e for e in eps if matches(e)]
        if not cand:
            print(f"\n[ERR] no endpoint matches '{args.match}'. Re-run with "
                  f"--match <substring> (see list above).")
            return 2
        active = [e for e in cand if e[2] == 1]
        if len(active) == 1 and not args.any_state:
            guid, name, state, fmt, _ = active[0]
            if len(cand) > 1:
                print(f"\n[NOTE] {len(cand)} matches, auto-selected the ACTIVE one.")
        elif len(cand) == 1:
            guid, name, state, fmt, _ = cand[0]
        else:
            print(f"\n[ERR] {len(cand)} matches for '{args.match}':")
            for g, n, s, _, _ in cand:
                print(f"  {g}  [{DEVICE_STATE.get(s, s)}] '{n}'")
            print("Refine --match, or pass --guid / --any-state.")
            return 2
    print(f"\nTarget: {guid}  '{name}'")

    dev_blob = pkey_device_format_blob(args.rate)
    oem_blob = pkey_oem_format_blob(args.rate)
    print(f"DeviceFormat: {args.rate} Hz, stereo, 24-bit in 32-bit slot (PCM, {len(dev_blob)}B)")
    print(f"OEMFormat   : {args.rate} Hz, stereo, 32-bit float (AudioEngine, {len(oem_blob)}B)")

    if not args.apply:
        print("\n[dry-run] nothing written. Re-run with --apply (Administrator).")
        return 0
    if not is_admin():
        print("[ERR] --apply needs Administrator. Right-click -> 'Run as "
              "administrator'.")
        return 1
    apply_default_format(guid, dev_blob, oem_blob)
    print("\nNext steps:")
    print("  1. Reboot (recommended), or restart audio service:")
    print("       net stop Audiosrv && net start Audiosrv")
    print("  2. Re-run: python tools\\usb_probe.py")
    print("  3. mmsys.cpl -> device Properties -> 'Advanced' tab should now")
    print("     list the default format; volume slider should move.")
    print("If GetMixFormat still fails, capture the NSH console EP0 log lines")
    print("(type=0xA1 req=0x02 RANGE / req=0x01 CUR on entity 1) and report.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
