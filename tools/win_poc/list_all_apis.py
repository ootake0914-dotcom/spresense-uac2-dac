import sounddevice as sd

print("Host APIs:")
for idx, api in enumerate(sd.query_hostapis()):
    print(f"[{idx}] {api['name']} (devices: {api['devices']})")

print("\nAll output devices:")
for idx, d in enumerate(sd.query_devices()):
    if d['max_output_channels'] > 0:
        api = sd.query_hostapis(d['hostapi'])['name']
        print(f"[{idx}] {api:20s} | {d['name']}")
