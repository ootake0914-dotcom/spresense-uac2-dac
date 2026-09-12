import time
import serial

port = 'COM6'
print(f"[CAPTURE] Opening {port} and triggering hardware reset...")

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.1
ser.dtr = True
ser.rts = True
ser.open()
time.sleep(0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.1)

print("[CAPTURE] Board reset. Recording boot and enumeration logs for 12 seconds...")

start = time.time()
with open("test_logs/boot_enum.log", "w", encoding="utf-8", errors="replace") as f:
    while time.time() - start < 12.0:
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
print("[CAPTURE] Done! Log saved to test_logs/boot_enum.log")
