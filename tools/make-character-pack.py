#!/usr/bin/env python3
"""Turn a folder of added characters into characters/added-characters.pack,
the file the builder (BUILD.cmd, builder/build.py) adds them from.

    python tools/import-characters.py --rom rom.z64 --patches <folder of .xdelta> --out out/characters
    python tools/make-character-pack.py --rom rom.z64 --characters out/characters

The folder (assets.bin, assets.lut.bin, characters.txt, voices/) is the
game's assets with the characters added, so it is mostly the ROM's own data
and cannot be given to anyone. The pack is the difference: every stretch that
is also in the ROM is written as "copy from the ROM", and only what is left,
the characters themselves, is in the pack. builder/characters.py has the
layout and puts the folder together again from the pack and a ROM.

Afterwards the pack is read back and compared with the folder, and its own
bytes are searched for anything of the ROM's.
"""
import argparse
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'builder'))
import characters      # noqa: E402
import rom as romfile  # noqa: E402

BLOCK, STEP = 32, 16        # stretches of the ROM are found by 32 bytes at every 16th place


def same_ahead(a, at_a, b, at_b):
    """How many bytes from a[at_a:] and b[at_b:] are the same."""
    most = min(len(a) - at_a, len(b) - at_b)
    done, chunk = 0, 65536
    while done < most:
        n = min(chunk, most - done)
        if a[at_a + done:at_a + done + n] == b[at_b + done:at_b + done + n]:
            done += n
        elif n == 1:
            break
        else:
            chunk = max(1, n // 2)
    return done


def same_behind(a, at_a, b, at_b, most):
    """How many bytes before a[at_a] and b[at_b] are the same, `most` at most."""
    n = 0
    while n < most and n < at_a and n < at_b and a[at_a - n - 1] == b[at_b - n - 1]:
        n += 1
    return n


def steps_of(data, rom, index):
    """`data` as steps: (place in the ROM, length) and (LITERAL, bytes)."""
    steps, at, own = [], 0, 0       # own: where the bytes not yet written begin
    end = len(data) - BLOCK
    while at <= end:
        found = index.get(data[at:at + BLOCK])
        if found is None:
            at += 1
            continue
        back = same_behind(data, at, rom, found, at - own)
        length = back + same_ahead(data, at, rom, found)
        if own < at - back:
            steps.append((characters.LITERAL, data[own:at - back]))
        steps.append((found - back, length))
        at = own = at - back + length
    if own < len(data):
        steps.append((characters.LITERAL, data[own:]))
    return steps


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rom', required=True, help='Diddy Kong Racing, US 1.0')
    p.add_argument('--characters', default=os.path.join(ROOT, 'out', 'characters'),
                   help='the folder tools/import-characters.py wrote (default: out/characters)')
    p.add_argument('--out', default=os.path.join(ROOT, 'characters', 'added-characters.pack'))
    args = p.parse_args()

    rom = romfile.load_rom(args.rom)
    names = ['assets.bin', 'assets.lut.bin', 'characters.txt']
    names += ['voices/' + name for name in sorted(os.listdir(os.path.join(args.characters, 'voices')))
              if name.lower().endswith('.bin')]
    index = {}
    for at in range(len(rom) - BLOCK, -1, -STEP):       # backwards: the first place of a block wins
        index[rom[at:at + BLOCK]] = at

    files, folder = [], {}
    for name in names:
        with open(os.path.join(args.characters, *name.split('/')), 'rb') as f:
            folder[name] = f.read()
        steps = steps_of(folder[name], rom, index)
        files.append((name, steps))
        own = sum(len(what) for where, what in steps if where == characters.LITERAL)
        print('%-28s %10d bytes, %10d of them not the ROM\'s, %d steps' % (name, len(folder[name]), own, len(steps)))

    pack = characters.write(rom, files)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(pack)

    # Read it back: the same files must come out of it.
    with open(args.out, 'rb') as f:
        made = characters.read(f.read(), rom)
    if made != folder:
        sys.exit('THE PACK IS NOT RIGHT: reading it back does not give the folder\'s files')
    # And nothing of the ROM may be in the pack's own bytes: no stretch of
    # them that the search above could have found.
    longest = 0
    for name, steps in files:
        for where, what in steps:
            if where == characters.LITERAL:
                for at in range(len(what) - BLOCK + 1):
                    if what[at:at + BLOCK] in index:
                        sys.exit('THE PACK IS NOT RIGHT: %s has the ROM\'s data in its own bytes' % name)
                longest = max(longest, len(what))
    print('\n%s: %.1f MB, %d characters: %s' % (os.path.relpath(args.out, ROOT), len(pack) / 1048576.0,
                                                len(characters.names(made)), ', '.join(characters.names(made))))
    print('read back: the same %d files (%.1f MB). None of the pack\'s own bytes is a stretch of the ROM\n'
          'of %d bytes or more.' % (len(made), sum(len(data) for data in made.values()) / 1048576.0, BLOCK + STEP - 1))
    print('SHA-1 of the pack: %s' % hashlib.sha1(pack).hexdigest())


if __name__ == '__main__':
    main()
