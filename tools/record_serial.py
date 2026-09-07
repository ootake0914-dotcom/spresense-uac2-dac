import os
import serial
import sys
import time

def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    outfile = sys.argv[2] if len(sys.argv) > 2 else "test_logs/spresense_serial.log"
    # UAC2_SERIAL_PORT overrides the default (e.g. /dev/ttyUSB0 on Linux).
    port = os.environ.get('UAC2_SERIAL_PORT', 'COM6')

    print(f"[SERIAL] Opening {port} (passive mode). Recording to {outfile} for {duration}s...", flush=True)
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        ser.dtr = False
        ser.rts = False

        start = time.time()
        with open(outfile, 'w', encoding='utf-8', errors='replace') as f:
            while time.time() - start < duration:
                line = ser.readline()
                if line:
                    try:
                        text = line.decode('utf-8', errors='replace')
                        f.write(text)
                        f.flush()
                        sys.stdout.write(f"[SPRESENSE] {text}")
                        sys.stdout.flush()
                    except Exception:
                        pass
        ser.close()
        print(f"[SERIAL] Recording finished. Saved to {outfile}", flush=True)
    except Exception as e:
        print(f"[SERIAL ERROR] {e}", flush=True)

if __name__ == '__main__':
    main()
