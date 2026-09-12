import time
import serial

port = 'COM6'
print(f"[CAPTURE] Opening {port} (passive mode) for 10 seconds...")

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
with open("test_logs/boot_enum_w5.log", "w", encoding="utf-8", errors="replace") as f:
    while time.time() - start < 10.0:
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
print("[CAPTURE] Done! Log saved to test_logs/boot_enum_w5.log")
