#!/bin/bash
set -e

echo "=== Building Spresense UAC2 192kHz/24bit DAC ==="
export PATH="/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin:/usr/bin:/bin"

cd /home/ootak/spresense/sdk

# Ensure uac2_dac is enabled in .config
if ! grep -q "CONFIG_EXAMPLES_UAC2_DAC=y" .config; then
    echo "[CONFIG] Enabling CONFIG_EXAMPLES_UAC2_DAC in .config..."
    echo "CONFIG_EXAMPLES_UAC2_DAC=y" >> .config
    echo "CONFIG_EXAMPLES_UAC2_DAC_PROGNAME=\"uac2_dac\"" >> .config
    echo "CONFIG_EXAMPLES_UAC2_DAC_PRIORITY=100" >> .config
    echo "CONFIG_EXAMPLES_UAC2_DAC_STACKSIZE=4096" >> .config
fi

# Build nuttx.spk
echo "[BUILD] Compiling nuttx and apps..."
rm -f nuttx.spk ../nuttx/nuttx ../nuttx/nuttx.spk
make -j8

echo "[FLASH] Flashing to Spresense on COM6..."
./tools/flash.sh -c COM6 -b 115200 nuttx.spk

echo "=== Build and Flash Complete! ==="
echo "Connect 115200bps serial terminal and run: nsh> uac2_dac"
