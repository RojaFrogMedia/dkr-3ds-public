#!/usr/bin/env python3
"""Add the adventure games of a DKR Recompiled (PC) save to the 3DS save file.

    python tools/import-save.py <3DS eeprom.bin>                   (from the newest PC save)
    python tools/import-save.py <pc save .bin> <3DS eeprom.bin>

The 3DS save is 3ds/DKR/eeprom.bin on the SD card.

Both are the N64 cartridge's EEPROM, 512 bytes: three adventure games of 40
bytes each (GAME A, B, C on the game select screen; an unused one is all FF),
then settings and records. A game's bytes are the same whichever place it is
in, so each game of the PC save is copied into a free place of the 3DS save.
Nothing already in the 3DS save is changed, and a copy of it is kept next to
it first (eeprom.bin.before-import). The game's program is not involved.
"""
import glob
import os
import shutil
import sys

GAME_BYTES, GAMES = 40, 3
PC_SAVES = os.path.expandvars(r'%APPDATA%\DKRPort\saves')


def games(data):
    return [data[i * GAME_BYTES:(i + 1) * GAME_BYTES] for i in range(GAMES)]


def unused(game):
    return game == b'\xff' * GAME_BYTES


def main():
    if len(sys.argv) == 3:
        source, target = sys.argv[1], sys.argv[2]
    elif len(sys.argv) == 2:
        found = glob.glob(os.path.join(PC_SAVES, '**', 'dkr.us.v77.bin'), recursive=True)
        if not found:
            sys.exit('no DKR Recompiled save under ' + PC_SAVES)
        source, target = max(found, key=os.path.getmtime), sys.argv[1]
    else:
        sys.exit(__doc__)
    pc = open(source, 'rb').read()
    mine = bytearray(open(target, 'rb').read())
    if len(pc) != 512 or len(mine) != 512:
        sys.exit('a save file is 512 bytes; these are %d and %d' % (len(pc), len(mine)))
    print('from', source)
    print('into', target)
    added = 0
    for number, game in enumerate(games(pc)):
        if unused(game):
            continue
        if game in games(mine):
            print('PC game %s is already in the 3DS save' % 'ABC'[number])
            continue
        free = [i for i, slot in enumerate(games(mine)) if unused(slot)]
        if not free:
            print('PC game %s: the 3DS save has no free game left' % 'ABC'[number])
            continue
        if added == 0:
            shutil.copyfile(target, target + '.before-import')
        mine[free[0] * GAME_BYTES:(free[0] + 1) * GAME_BYTES] = game
        added += 1
        print('PC game %s is now 3DS GAME %s' % ('ABC'[number], 'ABC'[free[0]]))
    if added:
        open(target, 'wb').write(mine)


if __name__ == '__main__':
    main()
