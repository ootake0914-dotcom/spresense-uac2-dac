#!/bin/bash
set -e

echo "[TEST] Sleeping 22s for Spresense reboot & enumeration..."
sleep 22

echo "[TEST] Connecting to yuhki machine..."
ssh -o BatchMode=yes yuhki@100.117.104.111 bash << 'YUHKEY'
set -e
if [ ! -f /sys/bus/usb/devices/3-3/devnum ]; then
    echo "[YUHKI] Error: 3-3 not found!"
    exit 1
fi
DEVNUM=$(cat /sys/bus/usb/devices/3-3/devnum)
DEVPATH=$(printf "/dev/bus/usb/003/%03d" $DEVNUM)
echo "[YUHKI] Spresense device is at $DEVPATH (devnum $DEVNUM)"

# Unbind snd-usb-audio if bound
for iface in 3-3:1.0 3-3:1.1; do
    if [ -d "/sys/bus/usb/drivers/snd-usb-audio/$iface" ]; then
        echo "[YUHKI] Unbinding $iface from snd-usb-audio..."
        echo "$iface" | sudo tee /sys/bus/usb/drivers/snd-usb-audio/unbind > /dev/null || true
    fi
done

# Start usbmon capture in background
echo "[YUHKI] Starting usbmon capture on 3u..."
sudo timeout 10 cat /sys/kernel/debug/usb/usbmon/3u > /tmp/usbmon_rev21.log 2>/dev/null &
USBMON_PID=$!
sleep 1

echo "[YUHKI] Running alt22.py with Alt 0, 1, 2, 0 via sudo..."
sudo python3 /tmp/alt22.py "$DEVPATH" 0 1 2 0

wait $USBMON_PID 2>/dev/null || true
echo "[YUHKI] Test done. Raw usbmon log for SET_INTERFACE:"
grep -E " 0b | -32|EPIPE" /tmp/usbmon_rev21.log || true
YUHKEY

echo "[TEST] Remote test completed successfully."
