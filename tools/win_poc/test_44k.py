import time
import numpy as np
import sounddevice as sd
import serial

# 44100Hz 2ch sine wave (1 sec)
fs = 44100
t = np.linspace(0, 1.0, fs, endpoint=False)
sine = (0.2 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
stereo_sine = np.column_stack((sine, sine))

print("Testing playback at 44100 Hz on device [3]...")
try:
    sd.play(stereo_sine, samplerate=fs, device=3)
    sd.wait()
    print(">>> 44.1kHz playback successful! <<<")
except Exception as e:
    print(f"Error at 44.1kHz: {e}")
