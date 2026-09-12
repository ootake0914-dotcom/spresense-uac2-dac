import time
import threading
import numpy as np
import sounddevice as sd
import serial

def serial_monitor(stop_evt):
    try:
        ser = serial.Serial()
        ser.port = 'COM6'
        ser.baudrate = 115200
        ser.timeout = 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
        ser.dtr = False
        ser.rts = False
        while not stop_evt.is_set():
            line = ser.readline()
            if line:
                try:
                    text = line.decode('utf-8', errors='replace').strip()
                    if text:
                        print(f"[FW] {text}", flush=True)
                except Exception:
                    pass
        ser.close()
    except Exception as e:
        print(f"[SERIAL ERR] {e}", flush=True)

stop_evt = threading.Event()
th = threading.Thread(target=serial_monitor, args=(stop_evt,))
th.daemon = True
th.start()

time.sleep(0.5)

# Try WDM-KS device 16
for fs in [192000, 48000, 44100]:
    t = np.linspace(0, 1.0, int(fs * 1.0), endpoint=False)
    sine = (0.2 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
    stereo_sine = np.column_stack((sine, sine))

    print(f"\n--- Testing WDM-KS device [16] at {fs} Hz ---", flush=True)
    try:
        sd.play(stereo_sine, samplerate=fs, device=16)
        sd.wait()
        print(f">>> WDM-KS playback at {fs}Hz succeeded! <<<", flush=True)
        break
    except Exception as e:
        print(f"Error at {fs}Hz: {e}", flush=True)

time.sleep(1.0)
stop_evt.set()
th.join(timeout=1.0)
