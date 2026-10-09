#!/usr/bin/env python3
"""The blank pictures and the chime that tools/make-prebuilt.sh packs into
builder/prebuilt/: a plain icon, an empty banner picture and the short sound
the HOME Menu plays with the banner. The builder later puts the real icon
and banner picture in, made from the ROM. Nothing here comes from the game.

    python3 tools/make-blank-art.py <folder>     writes icon.png, banner.png, banner.wav
"""
import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'builder'))
import picture  # noqa: E402


def chime(path, rate=32000):
    """A short rising chime, two channels, under two seconds."""
    notes = ((523.25, 0.00), (659.25, 0.11), (783.99, 0.22), (1046.50, 0.33))
    length = 1.6
    samples = []
    for i in range(int(rate * length)):
        t = i / rate
        value = 0.0
        for frequency, start in notes:
            if t < start:
                continue
            age = t - start
            envelope = min(1.0, age / 0.005) * math.exp(-age * 4.5)
            value += envelope * (math.sin(2 * math.pi * frequency * age) + 0.3 * math.sin(4 * math.pi * frequency * age))
        value *= min(1.0, (length - t) / 0.1)
        samples.append(int(max(-1.0, min(1.0, value * 0.22)) * 32767))
    with wave.open(path, 'wb') as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b''.join(struct.pack('<hh', s, s) for s in samples))


def main():
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'icon.png'), 'wb') as f:
        f.write(picture.Picture(48, 48, colour=(40, 80, 170, 255)).png())
    with open(os.path.join(folder, 'banner.png'), 'wb') as f:
        f.write(picture.Picture(256, 128).png())
    chime(os.path.join(folder, 'banner.wav'))


if __name__ == '__main__':
    main()
