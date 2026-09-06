#!/bin/bash
set -e

TEST_HOST="${UAC2_TEST_HOST:?Set UAC2_TEST_HOST (e.g. user@linux-host) for remote audio tests}"

ssh -o BatchMode=yes "$TEST_HOST" bash << 'REMOTE_EOF'
cat << 'PY_EOF' > /tmp/strict_probe.py
import fcntl, os, sys, struct, time

dev = sys.argv[1]
fd = os.open(dev, os.O_RDWR)
USBDEVFS_SETINTERFACE = 0x80085504

test_cases = [
    (1, 0, 'Interface 1, Alt 0 (Zero-BW Standby)'),
    (1, 1, 'Interface 1, Alt 1 (16-bit PCM)'),
    (1, 2, 'Interface 1, Alt 2 (24-bit PCM)'),
    (1, 0, 'Interface 1, Alt 0 (Back to Standby)'),
]

print("=== STARTING SET_INTERFACE STRICT PROBE ===", flush=True)
for ifno, alt, desc in test_cases:
    print(f"\n>>> Sending SET_INTERFACE: {desc} (if={ifno}, alt={alt})", flush=True)
    t0 = time.time()
    try:
        fcntl.ioctl(fd, USBDEVFS_SETINTERFACE, struct.pack('II', ifno, alt))
        elapsed_us = (time.time() - t0) * 1e6
        print(f"    RESULT: SUCCESS (ACK) in {elapsed_us:.1f} us", flush=True)
    except OSError as e:
        elapsed_us = (time.time() - t0) * 1e6
        print(f"    RESULT: ERROR {e.errno} ({os.strerror(e.errno)}) in {elapsed_us:.1f} us", flush=True)
    time.sleep(1.0)

os.close(fd)
print("\n=== PROBE COMPLETE ===", flush=True)
PY_EOF

# Unbind kernel driver if attached
echo "3-3:1.0" | sudo tee /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null || true
echo "3-3:1.1" | sudo tee /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null || true

DEVNUM=$(cat /sys/bus/usb/devices/3-3/devnum)
DEVPATH=$(printf "/dev/bus/usb/003/%03d" $DEVNUM)
echo "[REMOTE] Target device: $DEVPATH (devnum $DEVNUM)"

# Start usbmon
sudo rm -f /tmp/usbmon_strict.log
sudo timeout 10 cat /sys/kernel/debug/usb/usbmon/3u > /tmp/usbmon_strict.log 2>/dev/null &
MON_PID=$!
sleep 1

# Run Python probe
sudo python3 /tmp/strict_probe.py "$DEVPATH"

wait $MON_PID 2>/dev/null || true
echo "[REMOTE] usbmon captured."
REMOTE_EOF
