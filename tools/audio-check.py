#!/usr/bin/env python3
"""Look at a recording the game made of its own sound (DKR_AUDIODUMP=1 tools/run-native.sh ...).

    python tools/audio-check.py [out/native-run/audio.raw] [seconds per line]

Writes the recording next to it as audio.wav to listen to, and prints how
loud each stretch is (0 is silence, 32767 the loudest possible) as a row of
bars, so that gaps, sounds that stop and silence are visible without ears.
The game mixes 22050 Hz stereo, 16 bit.
"""
import array, math, os, sys, wave

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("out", "native-run", "audio.raw")
step = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
data = array.array("h")
data.frombytes(open(path, "rb").read())
with wave.open(os.path.splitext(path)[0] + ".wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(22050)
    w.writeframes(data.tobytes())
chunk = int(22050 * step) * 2
for i in range(0, len(data) - chunk + 1, chunk):
    part = data[i:i + chunk]
    rms = math.sqrt(sum(v * v for v in part) / len(part))
    print("%6.1f s  %6d  %s" % (i / 2 / 22050.0, rms, "#" * int(rms / 150)))
