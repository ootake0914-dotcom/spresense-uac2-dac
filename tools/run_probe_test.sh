#!/bin/bash
set -e

ssh -o BatchMode=yes yuhki@100.117.104.111 bash << 'REMOTE_EOF'
cat << 'PY_EOF' > /tmp/probe_if_alt.py
import fcntl, os, sys, struct, time

dev = sys.argv[1]
fd = os.open(dev, os.O_RDWR)
USBDEVFS_SETINTERFACE = 0x80085504

test_cases = [
    (0, 0, 'if=0 alt=0 (valid AC)'),
    (0, 1, 'if=0 alt=1 (invalid AC alt)'),
    (1, 0, 'if=1 alt=0 (valid AS standby)'),
    (1, 1, 'if=1 alt=1 (valid AS 16bit)'),
    (1, 2, 'if=1 alt=2 (valid AS 24bit)'),
    (2, 0, 'if=2 alt=0 (non-existent IF)'),
]

for ifno, alt, desc in test_cases:
    try:
        fcntl.ioctl(fd, USBDEVFS_SETINTERFACE, struct.pack('II', ifno, alt))
        print(f"PROBE [{desc}]: SUCCESS (ACK)", flush=True)
    except OSError as e:
        print(f"PROBE [{desc}]: ERROR {e.errno} ({os.strerror(e.errno)})", flush=True)
    time.sleep(0.5)

os.close(fd)
PY_EOF

# Unbind kernel driver if attached
echo "3-3:1.0" | sudo tee /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null || true
echo "3-3:1.1" | sudo tee /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null || true

DEVNUM=$(cat /sys/bus/usb/devices/3-3/devnum)
DEVPATH=$(printf "/dev/bus/usb/003/%03d" $DEVNUM)
echo "[YUHKI] Testing device at $DEVPATH (devnum $DEVNUM)"

sudo timeout 6 cat /sys/kernel/debug/usb/usbmon/3u > /tmp/usbmon_probe.log 2>/dev/null &
MON_PID=$!
sleep 0.5
sudo python3 /tmp/probe_if_alt.py "$DEVPATH"
wait $MON_PID 2>/dev/null || true

echo "=== USBMON RESULTS ==="
grep -E " 0b | -32|EPIPE" /tmp/usbmon_probe.log || true
REMOTE_EOF
