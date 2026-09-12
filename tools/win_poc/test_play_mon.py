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

fs = 44100
t = np.linspace(0, 1.5, int(fs * 1.5), endpoint=False)
sine = (0.2 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
stereo_sine = np.column_stack((sine, sine))

print(f"\n--- Starting 44.1kHz playback on device 3 ---", flush=True)
try:
    sd.play(stereo_sine, samplerate=fs, device=3)
    sd.wait()
    print(">>> 44.1kHz playback finished without exception! <<<", flush=True)
except Exception as e:
    print(f"Play error: {e}", flush=True)

time.sleep(1.0)
stop_evt.set()
th.join(timeout=1.0)
