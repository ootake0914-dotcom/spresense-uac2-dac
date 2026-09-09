#!/bin/bash
# PC-side daily setup for Spresense UAC2 192kHz/24bit DAC.
# - Forces the analog-stereo profile (PipeWire sometimes picks iec958,
#   whose start times out on this single-Alt-0 device).
# - Sets the DAC as the default sink.
# - Pins USB autosuspend off (persistent via udev rule, installed once).
set -e

CARD="alsa_card.usb-Sony_Spresense_192kHz_24bit_Audio_0005-00"
SINK="alsa_output.usb-Sony_Spresense_192kHz_24bit_Audio_0005-00.analog-stereo"

if ! pactl list cards short | grep -q "$CARD"; then
  echo "[PC-DAC] Card not found. Plug the extension-board micro-USB first."
  exit 1
fi

echo "[PC-DAC] Forcing analog-stereo profile..."
pactl set-card-profile "$CARD" output:analog-stereo

echo "[PC-DAC] Setting default sink..."
pactl set-default-sink "$SINK"
pactl get-default-sink

# Autosuspend off now (udev rule keeps it across replugs).
for dev in /sys/bus/usb/devices/*/idProduct; do
  if [ "$(cat "$dev" 2>/dev/null)" = "0ced" ]; then
    d=$(dirname "$dev")
    if [ -w "$d/power/control" ]; then
      echo on > "$d/power/control" 2>/dev/null || true
      echo "[PC-DAC] autosuspend off for $d"
    fi
  fi
done

if [ ! -f /etc/udev/rules.d/99-spresense-dac.rules ]; then
  echo "[PC-DAC] Installing udev rule (needs sudo)..."
  echo 'ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="0ced", ATTR{power/control}="on"' \
    | sudo tee /etc/udev/rules.d/99-spresense-dac.rules > /dev/null
  sudo udevadm control --reload-rules
else
  echo "[PC-DAC] udev rule already installed."
fi

echo "[PC-DAC] Done. Play something and check the DAC."
