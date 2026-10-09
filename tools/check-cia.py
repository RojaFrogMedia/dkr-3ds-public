#!/usr/bin/env python3
"""Reads a DKR.cia back and says what a console will find in it.

    python tools/check-cia.py [FILE.cia] [--out out/menu] [--cxi out/menu/DKR.cxi]

(default file: output/sdcard/cias/DKR.cia, the builder's.) Prints the
title's number, its names, what the title asks of a New 3DS and what the
banner holds, checks every check sum and signature in the file, and writes
check-icon.png and check-banner.png: the icon and the banner's picture
decoded from the file, as the menus will show them. Ends with
"check-cia: ok" or says what is wrong.

--cxi also writes the program as the console installs it, which the
emulator runs as it is (DKR_RUN=out/menu/DKR.cxi tools/run-native.sh ...).

The HOME Menu itself cannot be run in the emulator, so this is the check
there is of the icon and banner short of installing the file on a console.
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'builder'))
import ctr          # noqa: E402
import picture      # noqa: E402


def untiled(data, width, height, colour):
    """A picture from the 3DS' tiled 16-bit pixels (the reverse of ctr.tiles)."""
    out = picture.Picture(width, height)
    for y in range(height):
        for x in range(width):
            index = ((((y >> 3) * (width >> 3) + (x >> 3)) << 6)
                     + ((x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3))
            out.pixels[4 * (y * width + x):4 * (y * width + x) + 4] = bytes(colour(struct.unpack_from('<H', data, 2 * index)[0]))
    return out


def rgb565(v):
    return (v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255


def rgba4444(v):
    return (v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('cia', nargs='?', default=os.path.join(ROOT, 'output', 'sdcard', 'cias', 'DKR.cia'))
    parser.add_argument('--out', default=os.path.join(ROOT, 'out', 'menu'))
    parser.add_argument('--cxi', help='also write the installed program to this file, for the emulator')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.cia, 'rb') as f:
        cia = ctr.Cia(f.read())
    problems = cia.faults()
    program = cia.content

    title = struct.unpack_from('<Q', program, 0x118)[0]
    product = program[0x150:0x160].rstrip(b'\0').decode()
    print('title %016X  product %s  program %d bytes' % (title, product, len(program)))
    if args.cxi:
        with open(args.cxi, 'wb') as f:
            f.write(program)

    # What the title asks for (the "extended header", after the program's header).
    ex = program[0x200:0x600]
    stack = struct.unpack_from('<I', ex, 0x1c)[0]
    flag0, new_mode, flag2 = ex[0x20c], ex[0x20d], ex[0x20e]
    fast, cache = bool(flag0 & 2), bool(flag0 & 1)
    print('name %s  stack %d KB  New 3DS: 804 MHz %s, L2 cache %s, memory mode %d  Old 3DS memory mode %d'
          % (ex[:8].rstrip(b'\0').decode(), stack // 1024, fast, cache, new_mode & 15, flag2 >> 4))
    if not (fast and cache):
        problems.append('the title does not ask for the New 3DS clock and cache')

    files = cia.program_files()
    print('in the title:', ', '.join('%s (%d)' % (name, len(data)) for name, data in files.items()))

    icon = files.get('icon', b'')
    if icon[:4] != b'SMDH':
        problems.append('no icon')
    else:
        def text(at, length):
            return icon[at:at + length].decode('utf-16-le').split('\0')[0]
        english = 8 + 0x200                     # the second of 16 languages
        flags = struct.unpack_from('<I', icon, 0x2028)[0]
        print('icon: "%s" / "%s" / "%s"  flags %#x' % (text(english, 0x80), text(english + 0x80, 0x100),
                                                      text(english + 0x180, 0x80), flags))
        if not flags & 1:
            problems.append('the icon is not marked visible: the HOME Menu would not show the title')
        large = untiled(icon[ctr.SMDH_LARGE:], 48, 48, rgb565)
        small = untiled(icon[ctr.SMDH_SMALL:ctr.SMDH_LARGE], 24, 24, rgb565)
        sheet = picture.Picture(48 * 6 + 48 + 24 + 32, 48 * 6 + 16, colour=(40, 40, 48, 255))
        sheet.draw(large.resized(48 * 6, 48 * 6), 8, 8)
        sheet.draw(large, 48 * 6 + 16, 8)
        sheet.draw(small, 48 * 6 + 16 + 56, 8)
        with open(os.path.join(args.out, 'check-icon.png'), 'wb') as f:
            f.write(sheet.png())

    banner = files.get('banner', b'')
    if banner[:4] != b'CBMD':
        problems.append('no banner')
    else:
        model, sound = ctr.banner_parts(banner)
        print('banner: model %d bytes, sound %s %d bytes' % (len(model), sound[:4].decode('latin-1'), len(sound)))
        if model[:4] != b'CGFX':
            problems.append('the banner has no model')
        if sound[:4] != b'CWAV':
            problems.append('the banner has no sound')
        if len(model) == ctr.BANNER_MODEL_BYTES + 2 * 256 * 128:
            shown = picture.Picture(256, 128, colour=(60, 60, 72, 255))
            shown.draw(untiled(model[ctr.BANNER_MODEL_BYTES:], 256, 128, rgba4444), 0, 0)
            with open(os.path.join(args.out, 'check-banner.png'), 'wb') as f:
                f.write(shown.png())

    for problem in problems:
        print('WRONG:', problem)
    print('check-cia: ok' if not problems else 'check-cia: %d wrong' % len(problems))
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
