#!/usr/bin/env python3
"""Read-only WASAPI probe of the default render endpoint.

Calls (no Start, no state changes):
  IMMDevice::GetId, IAudioClient::GetMixFormat,
  IsFormatSupported(SHARED/EXCLUSIVE, 192k/24-in-32),
  GetDevicePeriod, shared Initialize (released immediately, never Started),
  IAudioEndpointVolume::GetChannelCount/GetMasterVolumeLevelScalar/GetMute.
Exit 0 if Initialize(SHARED) succeeds, 1 otherwise.

Windows-only, stdlib only (ctypes COM, no extra packages).
Run from an interactive Windows terminal (CMD/PowerShell), NOT from WSL:
    python tools\\wasapi_probe.py
"""
import ctypes
import os
import sys
import uuid
from ctypes import wintypes

ole32 = ctypes.windll.ole32

S_OK = 0
REGDB_E_CLASSNOTREG = 0x80040154
COINIT_MULTITHREADED = 0
CLSCTX_ALL = 0x17
AUDCLNT_SHAREMODE_SHARED = 0
AUDCLNT_SHAREMODE_EXCLUSIVE = 1


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


def _mmde_via_dll(clsid, iid):
    """Activate MMDeviceEnumerator via mmdevapi.dll!DllGetClassObject.

    Bypasses HKCR (seen missing on some Win11 24H2 installs) by using the
    in-proc class factory directly. Returns (interface_ptr, hresult).
    """
    from ctypes import wintypes as _wt
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
    # IClassFactory::CreateInstance (vtbl 3)
    obj = ctypes.c_void_p()
    r = _vtbl(fac, 3, ctypes.HRESULT, ctypes.c_void_p, ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(fac, None, ctypes.byref(iid),
                                               ctypes.byref(obj))
    _vtbl(fac, 2, ctypes.ULONG)(fac)  # Release factory
    return obj, r


def main():
    ole32.CoInitializeEx(None, COINIT_MULTITHREADED)
    try:
        return run()
    finally:
        ole32.CoUninitialize()


def run():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fix_shared_mode as fsm

    # --- enumerator + default endpoint (eConsole) ---
    CLSID_MMDE = GUID("BCDE0395-E52F-4A43-ABE7-D9E29BFCAC0F")
    IID_IMMDE = GUID("A95664D2-9614-4F35-A746-DE8DB636B871")
    enum = ctypes.c_void_p()
    try:
        r = ole32.CoCreateInstance(ctypes.byref(CLSID_MMDE), None, CLSCTX_ALL,
                                   ctypes.byref(IID_IMMDE), ctypes.byref(enum))
    except OSError as e:
        r = e.winerror
    print(f"CoCreateInstance(MMDeviceEnumerator) = {hr(r)}")
    if r != 0:
        if (r & 0xFFFFFFFF) == REGDB_E_CLASSNOTREG:
            print("[INFO] HKCR registration missing; falling back to")
            print("       mmdevapi.dll!DllGetClassObject (bypasses registry)...")
            enum, r = _mmde_via_dll(CLSID_MMDE, IID_IMMDE)
            print(f"DllGetClassObject fallback = {hr(r)}")
        if r != 0:
            return 1
    dev = ctypes.c_void_p()
    r = _vtbl(enum, 4, ctypes.HRESULT, wintypes.UINT, wintypes.UINT,
              ctypes.POINTER(ctypes.c_void_p))(enum, 0, 0, ctypes.byref(dev))
    print(f"GetDefaultAudioEndpoint(eRender,eConsole) = {hr(r)}")
    if r != 0:
        return 1

    wid = ctypes.c_wchar_p()
    r = _vtbl(dev, 5, ctypes.HRESULT,
              ctypes.POINTER(ctypes.c_wchar_p))(dev, ctypes.byref(wid))
    epid = wid.value
    print(f"endpoint id: {epid}")
    ole32.CoTaskMemFree(wid)
    if "8BC47233" not in epid.upper():
        print("[WARN] default endpoint is NOT the 0CEB Spresense key!")

    # --- IAudioClient ---
    IID_AC = GUID("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")
    ac = ctypes.c_void_p()
    r = _vtbl(dev, 3, ctypes.HRESULT, ctypes.POINTER(GUID), wintypes.DWORD,
              ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(dev, ctypes.byref(IID_AC),
                                               CLSCTX_ALL, None, ctypes.byref(ac))
    print(f"Activate(IAudioClient) = {hr(r)}")
    if r != 0:
        return 1

    fmt = fsm.wave_format_extensible_24in32(192000)
    wfx = ctypes.create_string_buffer(bytes(fmt), len(fmt))

    for mode, name in ((AUDCLNT_SHAREMODE_SHARED, "SHARED"),
                       (AUDCLNT_SHAREMODE_EXCLUSIVE, "EXCLUSIVE")):
        r = _vtbl(ac, 7, ctypes.HRESULT, wintypes.UINT, ctypes.c_void_p,
                  ctypes.c_void_p)(ac, mode, wfx, None)
        print(f"IsFormatSupported({name},192k/24) = {hr(r)}")

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

    dflt = ctypes.c_int64()
    minimum = ctypes.c_int64()
    r = _vtbl(ac, 9, ctypes.HRESULT, ctypes.POINTER(ctypes.c_int64),
              ctypes.POINTER(ctypes.c_int64))(ac, ctypes.byref(dflt), ctypes.byref(minimum))
    print(f"GetDevicePeriod = {hr(r)}" + (f" (default={dflt.value/10000:.2f}ms min={minimum.value/10000:.2f}ms)" if r == 0 else ""))

    r = _vtbl(ac, 3, ctypes.HRESULT, wintypes.UINT, wintypes.DWORD,
              ctypes.c_int64, ctypes.c_int64, ctypes.c_void_p,
              ctypes.c_void_p)(ac, AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, None, None)
    print(f"Initialize(SHARED,20ms) = {hr(r)}  [released immediately, never Started]")
    init_ok = (r == 0)

    # --- IAudioEndpointVolume (read-only) ---
    IID_EV = GUID("5CDF2C82-841E-4546-9722-0CF74078229A")
    ev = ctypes.c_void_p()
    r = _vtbl(dev, 3, ctypes.HRESULT, ctypes.POINTER(GUID), wintypes.DWORD,
              ctypes.c_void_p,
              ctypes.POINTER(ctypes.c_void_p))(dev, ctypes.byref(IID_EV),
                                               CLSCTX_ALL, None, ctypes.byref(ev))
    print(f"Activate(IAudioEndpointVolume) = {hr(r)}")
    if r == 0:
        nch = wintypes.UINT()
        r = _vtbl(ev, 5, ctypes.HRESULT,
                  ctypes.POINTER(wintypes.UINT))(ev, ctypes.byref(nch))
        print(f"  GetChannelCount = {hr(r)}" + (f" ({nch.value}ch)" if r == 0 else ""))
        lvl = ctypes.c_float()
        r = _vtbl(ev, 9, ctypes.HRESULT,
                  ctypes.POINTER(ctypes.c_float))(ev, ctypes.byref(lvl))
        print(f"  GetMasterVolumeLevelScalar = {hr(r)}" + (f" ({lvl.value:.3f})" if r == 0 else ""))
        mute = wintypes.BOOL()
        r = _vtbl(ev, 15, ctypes.HRESULT,
                  ctypes.POINTER(wintypes.BOOL))(ev, ctypes.byref(mute))
        print(f"  GetMute = {hr(r)}" + (f" ({bool(mute.value)})" if r == 0 else ""))
        _vtbl(ev, 2, ctypes.ULONG)(ev)
    _vtbl(ac, 2, ctypes.ULONG)(ac)
    _vtbl(dev, 2, ctypes.ULONG)(dev)
    _vtbl(enum, 2, ctypes.ULONG)(enum)
    print("WASAPI probe done (no stream was Started, no settings changed)")
    return 0 if init_ok else 1


if __name__ == "__main__":
    sys.exit(main())
