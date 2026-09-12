import time
import serial

port = 'COM6'
print(f"[MONITOR] Opening {port} (passive mode). Listening for AudioSrv / Playback events for 30s...")

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.1
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False

start = time.time()
with open("test_logs/monitor_audiosrv.log", "w", encoding="utf-8", errors="replace") as f:
    while time.time() - start < 30.0:
        line = ser.readline()
        if line:
            try:
                text = line.decode('utf-8', errors='replace')
                f.write(text)
                f.flush()
                print(text, end="", flush=True)
            except Exception:
                pass

ser.close()
print("[MONITOR] Finished. Saved to test_logs/monitor_audiosrv.log")
