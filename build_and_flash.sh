#!/bin/bash
set -e

echo "=== Building Spresense UAC2 192kHz/24bit DAC (Phase 1: Enumeration) ==="
export PATH="/home/ootak/spresense-tools/gcc-arm-none-eabi-9-2020-q2-update/bin:/usr/bin:/bin"

SPRESENSE=/home/ootak/spresense
SDK=$SPRESENSE/sdk
NUTTX=$SPRESENSE/nuttx
DOTCONFIG=$NUTTX/.config
APP_LINK=$SDK/apps/examples/uac2_dac
APP_SRC=/home/ootak/spresense_uac2_dac

# 1. Ensure symlink apps/examples/uac2_dac -> spresense_uac2_dac
if [ ! -e "$APP_LINK" ]; then
    echo "[LINK] Creating symlink $APP_LINK -> $APP_SRC"
    ln -s "$APP_SRC" "$APP_LINK"
else
    echo "[LINK] OK: $APP_LINK exists ($(readlink $APP_LINK))"
fi

cd $SDK

# 2. Ensure required Kconfig options in nuttx/.config
#    Phase 1 needs USB device controller + USBDEV core. ISOC EPs (Phase 2)
#    need a cxd56_usbdev.c patch (see README Phase 2 notes), but enumeration
#    itself works without it.
if [ ! -f "$DOTCONFIG" ]; then
    echo "[ERROR] $DOTCONFIG not found. Run tools/config.py first (e.g. ./tools/config.py default)."
    exit 1
fi

ensure_config() {
    key="$1"
    val="$2"
    if ! grep -q "^${key}=" "$DOTCONFIG" 2>/dev/null; then
        echo "[CONFIG] Adding ${key}=${val}"
        echo "${key}=${val}" >> "$DOTCONFIG"
    else
        echo "[CONFIG] OK: ${key} already set ($(grep "^${key}=" "$DOTCONFIG"))"
    fi
}

enable_config() {
    key="$1"
    if grep -q "^# ${key} is not set" "$DOTCONFIG" 2>/dev/null; then
        echo "[CONFIG] Enabling ${key} (was not set)"
        sed -i "s/^# ${key} is not set/${key}=y/" "$DOTCONFIG"
    fi
    ensure_config "$key" "y"
}

disable_config() {
    key="$1"
    if grep -q "^${key}=" "$DOTCONFIG" 2>/dev/null; then
        echo "[CONFIG] Disabling ${key}"
        sed -i "s/^${key}=.*/# ${key} is not set/" "$DOTCONFIG"
    fi
}

# 1. Disable MIDI Synth & Multicore to free SRAM
echo "[CONFIG] Disabling HexaMIDI (CONFIG_EXAMPLES_SYNTH) for dedicated UAC2 DAC..."
disable_config "CONFIG_EXAMPLES_SYNTH"
disable_config "CONFIG_EXAMPLES_SYNTH_MULTICORE"

# 2. Set boot entrypoint to uac2_dac_main
echo "[CONFIG] Setting boot entrypoint to uac2_dac_main..."
sed -i 's/CONFIG_INIT_ENTRYPOINT=.*/CONFIG_INIT_ENTRYPOINT="uac2_dac_main"/' "$DOTCONFIG"
sed -i 's/CONFIG_INIT_ENTRYNAME=.*/CONFIG_INIT_ENTRYNAME="uac2_dac_main"/' "$DOTCONFIG"
sed -i 's/CONFIG_INIT_STACKSIZE=.*/CONFIG_INIT_STACKSIZE=8192/' "$DOTCONFIG"

# 3. Enable USB device & UAC2 DAC
enable_config "CONFIG_CXD56_USBDEV"
enable_config "CONFIG_USBDEV"
ensure_config "CONFIG_USBDEV_DMA" "y"
ensure_config "CONFIG_USBDEV_DUALSPEED" "y"
ensure_config "CONFIG_USBDEV_SELFPOWERED" "y"
ensure_config "CONFIG_USBDEV_MAXPOWER" "100"
enable_config "CONFIG_EXAMPLES_UAC2_DAC"
ensure_config "CONFIG_EXAMPLES_UAC2_DAC_PROGNAME" "\"uac2_dac\""
ensure_config "CONFIG_EXAMPLES_UAC2_DAC_PRIORITY" "100"
ensure_config "CONFIG_EXAMPLES_UAC2_DAC_STACKSIZE" "8192"

# Refresh auto-generated Kconfig (picks up uac2_dac/Kconfig via symlink) and
# resolve dependencies without prompting.
echo "[CONFIG] Running olddefconfig..."
make olddefconfig

# 3. Build nuttx.spk
echo "[BUILD] Compiling nuttx and apps..."
make -j8

# nuttx.spk is generated in sdk/ (Spresense SDK layout)
if [ ! -f nuttx.spk ]; then
    echo "[ERROR] nuttx.spk not found after build."
    exit 1
fi

# 4. Flash (WSL: pass Windows COM port through; Linux: /dev/ttyUSB0)
PORT="${1:-COM6}"
echo "[FLASH] Flashing to Spresense on $PORT..."
./tools/flash.sh -c "$PORT" -b 115200 nuttx.spk

echo "=== Build and Flash Complete! ==="
echo "1. Connect 115200bps serial terminal (spresense main console)."
echo "2. Run:  nsh> uac2_dac"
echo "3. Connect Spresense USB (device port) to PC."
echo "4. On PC run:  python3 tools/usb_probe.py   (or check Sound settings)"
echo "   Expect: 'Spresense 192kHz/24bit Audio' with 192000 Hz, 2ch, 24-bit."

