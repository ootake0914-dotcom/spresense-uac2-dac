import sounddevice as sd

for i, d in enumerate(sd.query_devices()):
    if 'Spresense' in d['name']:
        api = sd.query_hostapis(d['hostapi'])['name']
        print(f"[{i}] {api:25s} | {d['name']} | out={d['max_output_channels']} | sr={d['default_samplerate']}")
