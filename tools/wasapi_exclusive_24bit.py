#!/usr/bin/env python3
"""Definitive UAC2 pin test: raw WASAPI on the Spresense endpoint (no PortAudio/MME).

Targets the ACTIVE Spresense render endpoint by GUID substring, then:
  1. GetMixFormat (shared engine format; needs the fix_shared_mode.py --apply state)
  2. IsFormatSupported(SHARED, 192k PCM 24-in-32)
  3. IsFormatSupported(EXCLUSIVE, 192k PCM 24-in-32)
  4. EXCLUSIVE Initialize + Start + 2s of silence (watch NSH for Alt:1 / Buf growth)
  5. EndpointVolume: GetVolumeRange / GetMasterVolumeLevelScalar / Set 0.5 / restore
     (watch NSH for SET_CUR volume: type=0x21 req=0x01 on entity 2)

Windows-only, stdlib only (ctypes COM). Run in an interactive terminal (CMD/PowerShell),
NOT from WSL:
    python tools\\wasapi_exclusive_24bit.py
    python tools\\wasapi_exclusive_24bit.py --match 4405314d --seconds 3

Exit 0 if EXCLUSIVE Initialize+Start succeeded.
"""
import argparse
import ctypes
import os
import sys
import time
import uuid
from ctypes import wintypes

ole32 = ctypes.windll.ole32
CLSCTX_ALL = 0x17
COINIT_MULTITHREADED = 0
AUDCLNT_SHAREMODE_SHARED = 0
AUDCLNT_SHAREMODE_EXCLUSIVE = 1
REGDB_E_CLASSNOTREG = 0x80040154
DEVICE_STATE_ACTIVE = 0x00000001


def _mmde_via_dll(clsid, iid):
    """Activate MMDeviceEnumerator via mmdevapi.dll!DllGetClassObject.

    Bypasses HKCR (missing on some Win11 24H2 installs).
    """
    try:
        mmdev = ctypes.WinDLL("mmdevapi.dll")
    except OSError as e:
        print(f"[ERR] cannot load mmdevapi.dll: {e}")
        return ctypes.c_void_p(), -1
    try:
        dgo = mmdev.DllGetClassObject
    except AttributeError:
        print("[ERR] mmdevapi.dll has no DllGetClassObject export")
        return ctypes.c_void_p(), -1
    dgo.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                    ctypes.POINTER(ctypes.c_void_p)]
    dgo.restype = ctypes.HRESULT
    IID_CF = GUID("00000001-0000-0000-C000-000000000046")
    fac = ctypes.c_void_p()
    r = dgo(ctypes.byref(clsid), ctypes.byref(IID_CF), ctypes.byref(fac))
    if r != 0:
        return ctypes.c_void_p(), r
    obj = ctypes.c_void_p()
    r = _vtbl(fac, 3, ctypes.HRESULT, ctypes.c_void_p, ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(fac, None, ctypes.byref(iid),
                                               ctypes.byref(obj))
    _vtbl(fac, 2, ctypes.ULONG)(fac)  # Release factory
    return obj, r


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", wintypes.BYTE * 8)]

    def __init__(self, s="00000000-0000-0000-0000-000000000000"):
        u = uuid.UUID(s)
        super().__init__(u.time_low, u.time_mid, u.time_hi_version,
                         (wintypes.BYTE * 8)(*u.bytes[8:]))


def _vtbl(obj, idx, restype, *argtypes):
    v = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0][idx]
    return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(v)


def hr(x):
    return f"0x{x & 0xFFFFFFFF:08X}" + (" (S_OK)" if x == 0 else "")


def main():
    ap = argparse.ArgumentParser(description="Raw WASAPI exclusive 24-bit pin test")
    ap.add_argument("--match", default="4405314d",
                    help="endpoint-ID substring (default: current Spresense GUID)")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--rate", type=int, default=192000)
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fix_shared_mode as fsm

    ole32.CoInitializeEx(None, COINIT_MULTITHREADED)
    try:
        return run(args, fsm)
    finally:
        ole32.CoUninitialize()


def run(args, fsm):
    CLSID_MMDE = GUID("BCDE0395-E52F-4A43-ABE7-D9E29BFCAC0F")
    IID_IMMDE = GUID("A95664D2-9614-4F35-A746-DE8DB636B871")
    enum = ctypes.c_void_p()
    r = ole32.CoCreateInstance(ctypes.byref(CLSID_MMDE), None, CLSCTX_ALL,
                               ctypes.byref(IID_IMMDE), ctypes.byref(enum))
    print(f"CoCreateInstance(MMDeviceEnumerator) = {hr(r)}")
    if r != 0:
        if (r & 0xFFFFFFFF) == REGDB_E_CLASSNOTREG:
            print("[INFO] HKCR registration missing; falling back to")
            print("       mmdevapi.dll!DllGetClassObject (bypasses registry)...")
            enum, r = _mmde_via_dll(CLSID_MMDE, IID_IMMDE)
            print(f"DllGetClassObject fallback = {hr(r)}")
        if r != 0:
            return 1

    # EnumAudioEndpoints(eRender=0, ACTIVE, collection)
    coll = ctypes.c_void_p()
    r = _vtbl(enum, 3, ctypes.HRESULT, wintypes.UINT, wintypes.DWORD,
              ctypes.POINTER(ctypes.c_void_p))(enum, 0, DEVICE_STATE_ACTIVE,
                                               ctypes.byref(coll))
    print(f"EnumAudioEndpoints(render,ACTIVE) = {hr(r)}")
    if r != 0:
        return 1
    n = wintypes.UINT()
    _vtbl(coll, 3, ctypes.HRESULT,
          ctypes.POINTER(wintypes.UINT))(coll, ctypes.byref(n))
    print(f"active render endpoint count = {n.value}")
    dev = None
    for i in range(n.value):
        d = ctypes.c_void_p()
        _vtbl(coll, 4, ctypes.HRESULT, wintypes.UINT,
              ctypes.POINTER(ctypes.c_void_p))(coll, i, ctypes.byref(d))
        wid = ctypes.c_wchar_p()
        _vtbl(d, 5, ctypes.HRESULT,
              ctypes.POINTER(ctypes.c_wchar_p))(d, ctypes.byref(wid))
        epid = wid.value or ""
        ole32.CoTaskMemFree(wid)
        mark = ""
        if args.match.lower() in epid.lower():
            mark = "  <== TARGET"
            dev = d
        else:
            _vtbl(d, 2, ctypes.ULONG)(d)
        print(f"  [{i}] {epid}{mark}")
    _vtbl(coll, 2, ctypes.ULONG)(coll)
    if dev is None:
        print(f"[ERR] no ACTIVE endpoint matches '{args.match}'.")
        return 2
    _vtbl(enum, 2, ctypes.ULONG)(enum)

    IID_AC = GUID("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")
    ac = ctypes.c_void_p()
    r = _vtbl(dev, 3, ctypes.HRESULT, ctypes.POINTER(GUID), wintypes.DWORD,
              ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(dev, ctypes.byref(IID_AC),
                                               CLSCTX_ALL, None, ctypes.byref(ac))
    print(f"Activate(IAudioClient) = {hr(r)}")
    if r != 0:
        return 1

    import struct
    pmix = ctypes.c_void_p()
    r = _vtbl(ac, 8, ctypes.HRESULT,
              ctypes.POINTER(ctypes.c_void_p))(ac, ctypes.byref(pmix))
    print(f"GetMixFormat = {hr(r)}")
    if r == 0 and pmix:
        tag, ch, rate, avg, blk, bits, cb = struct.unpack(
            "<HHIIHHH", ctypes.string_at(pmix, 18))
        print(f"  mix: tag={tag:#x} ch={ch} rate={rate} bits={bits} blk={blk}")
        ole32.CoTaskMemFree(pmix)

    fmt = fsm.wave_format_extensible_24in32(args.rate)
    wfx = ctypes.create_string_buffer(bytes(fmt), len(fmt))
    for mode, name in ((AUDCLNT_SHAREMODE_SHARED, "SHARED"),
                       (AUDCLNT_SHAREMODE_EXCLUSIVE, "EXCLUSIVE")):
        rr = _vtbl(ac, 7, ctypes.HRESULT, wintypes.UINT, ctypes.c_void_p,
                   ctypes.c_void_p)(ac, mode, wfx, None)
        print(f"IsFormatSupported({name},{args.rate}/24) = {hr(rr)}")

    # EXCLUSIVE open + Start + silence
    hns = int(args.seconds * 10000000)
    r = _vtbl(ac, 3, ctypes.HRESULT, wintypes.UINT, wintypes.DWORD,
              ctypes.c_int64, ctypes.c_int64, ctypes.c_void_p,
              ctypes.c_void_p)(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, hns, 0, wfx, None)
    print(f"Initialize(EXCLUSIVE,{args.rate}/24,{args.seconds}s) = {hr(r)}")
    if r != 0:
        _vtbl(ac, 2, ctypes.ULONG)(ac)
        _vtbl(dev, 2, ctypes.ULONG)(dev)
        return 1

    nframes = wintypes.UINT()
    _vtbl(ac, 4, ctypes.HRESULT,
          ctypes.POINTER(wintypes.UINT))(ac, ctypes.byref(nframes))
    print(f"GetBufferSize = {nframes.value} frames")
    IID_RC = GUID("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")
    rc = ctypes.c_void_p()
    r = _vtbl(ac, 14, ctypes.HRESULT, ctypes.POINTER(GUID), wintypes.DWORD,
              ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(ac, ctypes.byref(IID_RC),
                                               CLSCTX_ALL, None, ctypes.byref(rc))
    print(f"GetService(IAudioRenderClient) = {hr(r)}")
    ok = False
    if r == 0:
        ptr = ctypes.c_void_p()
        r = _vtbl(rc, 3, ctypes.HRESULT, wintypes.UINT,
                  ctypes.POINTER(ctypes.c_void_p))(rc, nframes.value,
                                                   ctypes.byref(ptr))
        print(f"GetBuffer({nframes.value}) = {hr(r)}")
        if r == 0:
            ctypes.memset(ptr, 0, nframes.value * 8)  # stereo 32-bit slots
            flags = wintypes.DWORD(0)
            r = _vtbl(rc, 4, ctypes.HRESULT, wintypes.UINT,
                      wintypes.DWORD)(rc, nframes.value, flags)
            print(f"ReleaseBuffer = {hr(r)}")
            r = _vtbl(ac, 10, ctypes.HRESULT)(ac)
            print(f"Start = {hr(r)}  [NSH should show Alt:1 / Buf growth]")
            if r == 0:
                time.sleep(args.seconds)
                r = _vtbl(ac, 11, ctypes.HRESULT)(ac)
                print(f"Stop = {hr(r)}")
                ok = True
        _vtbl(rc, 2, ctypes.ULONG)(rc)

    # EndpointVolume read/write (Level-tab path)
    IID_EV = GUID("5CDF2C82-841E-4546-9722-0CF74078229A")
    ev = ctypes.c_void_p()
    r = _vtbl(dev, 3, ctypes.HRESULT, ctypes.POINTER(GUID), wintypes.DWORD,
              ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(dev, ctypes.byref(IID_EV),
                                               CLSCTX_ALL, None, ctypes.byref(ev))
    print(f"Activate(IAudioEndpointVolume) = {hr(r)}")
    if r == 0:
        lvl = ctypes.c_float()
        _vtbl(ev, 9, ctypes.HRESULT,
              ctypes.POINTER(ctypes.c_float))(ev, ctypes.byref(lvl))
        orig = lvl.value
        print(f"  volume scalar before = {orig:.3f}")
        r = _vtbl(ev, 7, ctypes.HRESULT, ctypes.c_float,
                  ctypes.c_void_p)(ev, ctypes.c_float(0.5), None)
        print(f"  SetMasterVolumeLevelScalar(0.5) = {hr(r)}")
        time.sleep(0.5)
        _vtbl(ev, 9, ctypes.HRESULT,
              ctypes.POINTER(ctypes.c_float))(ev, ctypes.byref(lvl))
        print(f"  volume scalar after = {lvl.value:.3f}")
        r = _vtbl(ev, 7, ctypes.HRESULT, ctypes.c_float,
                  ctypes.c_void_p)(ev, ctypes.c_float(orig), None)
        print(f"  restore({orig:.3f}) = {hr(r)}")
        _vtbl(ev, 2, ctypes.ULONG)(ev)
    _vtbl(ac, 2, ctypes.ULONG)(ac)
    _vtbl(dev, 2, ctypes.ULONG)(dev)
    print("done")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
