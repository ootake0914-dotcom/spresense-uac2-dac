import sounddevice as sd

for i, d in enumerate(sd.query_devices()):
    if d['hostapi'] == 2: # WASAPI
        print(f"[{i}] {d['name']} | in={d['max_input_channels']} | out={d['max_output_channels']} | sr={d['default_samplerate']}")
