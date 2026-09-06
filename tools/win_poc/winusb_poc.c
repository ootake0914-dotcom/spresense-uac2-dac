/* WinUSB PoC: stream a 192kHz/S32/stereo WAV to Spresense UAC2 Alt0 ISO OUT.
 *
 * No WDK needed (plain Windows SDK + MSVC). Device must first be bound to
 * WinUSB via Zadig (Options -> List All Devices -> Spresense -> WinUSB).
 *
 * Protocol: Alt0-only (never SET_INTERFACE alt>0). Byte stream in, 125us
 * microframes out; firmware eats any chunking (partial-tolerant ring).
 * Pacing is self-clocked by transfer completions (= SOF rate).
 *
 * Build (x64 Native / BuildTools):
 *   cl /O2 /W3 winusb_poc.c /link setupapi.lib winusb.lib
 *
 * Usage: winusb_poc.exe [wav] [gain 0.0-1.0, default 0.1]
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <winusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

#define VID "vid_054c"
#define PID "pid_0ced"
#define XFER_BYTES 4800u   /* 25 microframes worth, any chunking OK */
#define INFLIGHT 4
#define WAV_HDR_MAX 4096u

static volatile int g_stop = 0;
static BOOL WINAPI ctrl_handler(DWORD t) { (void)t; g_stop = 1; return TRUE; }

/* Minimal RIFF walk: returns offset+size of 'data' chunk, 0 on failure. */
static int wav_data_chunk(const uint8_t *f, size_t flen, size_t *off, size_t *n)
{
    size_t p;
    if (flen < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4))
        return 0;
    p = 12;
    while (p + 8 <= flen)
        {
            uint32_t id = *(uint32_t *)(f + p);
            uint32_t sz = *(uint32_t *)(f + p + 4);
            if (id == 0x61746164u) /* 'data' LE */
                {
                    *off = p + 8;
                    *n = sz;
                    if (*off + *n > flen) *n = flen - *off;
                    return 1;
                }
            p += 8 + ((sz + 1u) & ~1u);
            if (p > flen) break;
        }
    return 0;
}

int main(int argc, char **argv)
{
    const char *wavpath = (argc > 1) ? argv[1] : "spresense_192k24b_1khz.wav";
    float gain = (argc > 2) ? (float)atof(argv[2]) : 0.1f;
    DWORD maxsec = (argc > 3) ? (DWORD)atoi(argv[3]) : 0; /* 0 = until Ctrl-C */
    FILE *fp = NULL;
    uint8_t *file = NULL;
    size_t flen = 0, doff = 0, dlen = 0, pos = 0;
    HDEVINFO devs = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    DWORD i, need = 0;
    char *detailbuf = NULL;
    HANDLE hdev = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE wusb = NULL;
    USB_INTERFACE_DESCRIPTOR idesc;
    WINUSB_PIPE_INFORMATION pipe;
    UCHAR outpipe = 0;
    uint8_t *xfer[INFLIGHT] = { 0 };
    WINUSB_ISOCH_BUFFER_HANDLE hIso[INFLIGHT] = { 0 };
    OVERLAPPED ov[INFLIGHT];
    int outstanding = 0, k;
    uint64_t total = 0;
    DWORD t0, tlast;
    int rc = 1;

    /* Load WAV fully, locate data chunk */
    fp = fopen(wavpath, "rb");
    if (!fp) { printf("open wav failed: %s\n", wavpath); return 1; }
    fseek(fp, 0, SEEK_END);
    flen = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    file = (uint8_t *)malloc(flen ? flen : 1);
    if (!file || fread(file, 1, flen, fp) != flen) { printf("wav read failed\n"); goto out; }
    fclose(fp); fp = NULL;
    if (!wav_data_chunk(file, flen, &doff, &dlen) || dlen < 8)
        { printf("wav parse failed (need 192k/S32/stereo PCM)\n"); goto out; }
    printf("wav data: %llu bytes @+%llu (gain %.2f)\n",
           (unsigned long long)dlen, (unsigned long long)doff, gain);

    /* Find our device path */
    devs = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) { printf("SetupDi failed %lu\n", GetLastError()); goto out; }
    for (i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &GUID_DEVINTERFACE_USB_DEVICE, i, &ifd); i++)
        {
            SetupDiGetDeviceInterfaceDetailA(devs, &ifd, NULL, 0, &need, NULL);
            free(detailbuf);
            detailbuf = (char *)malloc(need);
            ((PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detailbuf)->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (!SetupDiGetDeviceInterfaceDetailA(devs, &ifd, (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detailbuf, need, NULL, NULL))
                continue;
            {
                char lower[1024];
                size_t c;
                const char *dp = ((PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detailbuf)->DevicePath;
                for (c = 0; c + 1 < sizeof(lower) && dp[c]; c++)
                    lower[c] = (char)tolower((unsigned char)dp[c]);
                lower[c] = 0;
                if (strstr(lower, VID) && strstr(lower, PID))
                    {
                        HANDLE h = CreateFileA(dp, GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
                        if (h == INVALID_HANDLE_VALUE) continue;
                        /* must be WinUSB-bound (composite parent also matches) */
                        {
                            WINUSB_INTERFACE_HANDLE w;
                            if (WinUsb_Initialize(h, &w)) { hdev = h; WinUsb_Free(w); break; }
                            CloseHandle(h);
                        }
                    }
            }
        }
    if (hdev == INVALID_HANDLE_VALUE) { printf("device 054c:0ced not found (Zadig WinUSB bound?)\n"); goto out; }
    printf("device opened\n");

    if (!WinUsb_Initialize(hdev, &wusb)) { printf("WinUsb_Initialize failed %lu\n", GetLastError()); goto out; }
    if (!WinUsb_QueryInterfaceSettings(wusb, 0, &idesc))
        { printf("QueryInterfaceSettings failed %lu\n", GetLastError()); goto out; }
    printf("interface %u, %u endpoints\n", idesc.bInterfaceNumber, idesc.bNumEndpoints);
    for (i = 0; i < idesc.bNumEndpoints; i++)
        {
            if (!WinUsb_QueryPipe(wusb, 0, (UCHAR)i, &pipe)) continue;
            printf(" pipe %u: type=%d id=0x%02x maxpkt=%u\n",
                   i, pipe.PipeType, pipe.PipeId, pipe.MaximumPacketSize);
            if (pipe.PipeType == UsbdPipeTypeIsochronous && !(pipe.PipeId & 0x80))
                outpipe = pipe.PipeId; /* e.g. 0x02 */
        }
    if (!outpipe) { printf("no ISO OUT pipe found\n"); goto out; }
    printf("using ISO OUT pipe 0x%02x\n", outpipe);

    for (k = 0; k < INFLIGHT; k++)
        {
            xfer[k] = (uint8_t *)malloc(XFER_BYTES);
            memset(&ov[k], 0, sizeof(ov[k]));
            ov[k].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            if (!xfer[k] || !ov[k].hEvent) { printf("alloc failed\n"); goto out; }
            if (!WinUsb_RegisterIsochBuffer(wusb, outpipe, xfer[k], XFER_BYTES, &hIso[k]))
                { printf("RegisterIsochBuffer failed %lu\n", GetLastError()); goto out; }
        }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("streaming... Ctrl-C to stop\n");
    t0 = tlast = GetTickCount();

    while (!g_stop)
        {
            /* top up in-flight transfers */
            while (outstanding < INFLIGHT && !g_stop)
                {
                    int slot = -1;
                    for (k = 0; k < INFLIGHT; k++)
                        if (WaitForSingleObject(ov[k].hEvent, 0) == WAIT_OBJECT_0) { slot = k; break; }
                    if (slot < 0) break;
                    {
                        /* fill with gain-scaled S32 samples, ring through file */
                        int32_t *dst = (int32_t *)xfer[slot];
                        DWORD s;
                        for (s = 0; s < XFER_BYTES / 4; s++)
                            {
                                int32_t v = *(int32_t *)(file + doff + pos);
                                dst[s] = (int32_t)(v * gain);
                                pos += 4;
                                if (pos + 4 > dlen) pos = 0;
                            }
                    }
                    ResetEvent(ov[slot].hEvent);
                    if (!WinUsb_WriteIsochPipeAsap(hIso[slot], 0, XFER_BYTES, TRUE, &ov[slot]) &&
                        GetLastError() != ERROR_IO_PENDING)
                        { printf("WriteIsochPipeAsap failed %lu\n", GetLastError()); goto out; }
                    outstanding++;
                }
            /* wait for at least one completion */
            {
                HANDLE ev[INFLIGHT];
                DWORD w;
                for (k = 0; k < INFLIGHT; k++) ev[k] = ov[k].hEvent;
                w = WaitForMultipleObjects(INFLIGHT, ev, FALSE, 1000);
                if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + INFLIGHT)
                    {
                        DWORD x = 0;
                        if (GetOverlappedResult(hdev, &ov[w - WAIT_OBJECT_0], &x, FALSE))
                            { total += x; outstanding--; }
                        else
                            { printf("transfer error %lu\n", GetLastError()); outstanding--; }
                    }
                else if (w == WAIT_TIMEOUT)
                    printf("stall: no completion in 1s (device wedged?)\n");
            }
            {
                DWORD now = GetTickCount();
                if (maxsec && now - t0 >= maxsec * 1000) { printf("time limit reached\n"); g_stop = 1; }
                if (now - tlast >= 1000)
                    {
                        double el = (now - t0) / 1000.0;
                        printf("[%.0fs] %llu bytes, avg %.1f kB/s (nominal 1536.0)\n",
                               el, (unsigned long long)total, total / el / 1024.0);
                        tlast = now;
                    }
            }
        }
    printf("stopped: %llu bytes total\n", (unsigned long long)total);
    rc = 0;

out:
    for (k = 0; k < INFLIGHT; k++) { if (hIso[k]) WinUsb_UnregisterIsochBuffer(hIso[k]); }
    if (wusb) { WinUsb_AbortPipe(wusb, outpipe); WinUsb_Free(wusb); }
    if (hdev != INVALID_HANDLE_VALUE) CloseHandle(hdev);
    for (k = 0; k < INFLIGHT; k++) { if (ov[k].hEvent) CloseHandle(ov[k].hEvent); free(xfer[k]); }
    free(detailbuf);
    if (devs != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(devs);
    if (fp) fclose(fp);
    free(file);
    return rc;
}
