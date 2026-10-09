#!/usr/bin/env python3
"""Makes Diddy Kong Racing for the Nintendo 3DS from your own ROM.

    python builder/build.py                     looks for the ROM in this folder and in rom/
    python builder/build.py "path/to/rom.z64"   or name it
    python builder/build.py --out somewhere     (default: output/)
    python builder/build.py --no-characters     the game without the added characters

On Windows, BUILD.cmd does this for you: double-click it, or drop the ROM on it.

What it writes (copy the contents of output/sdcard/ onto the SD card):

    output/sdcard/3ds/DKR/DKR.3dsx          the game, for the Homebrew Launcher
    output/sdcard/3ds/DKR/assets.bin        the game's data, taken out of the ROM
    output/sdcard/3ds/DKR/assets.lut.bin
    output/sdcard/3ds/DKR/characters.txt    the added characters (characters/added-characters.pack):
    output/sdcard/3ds/DKR/voices/*.bin      their list and their voices
    output/sdcard/3ds/DKR/bottom/*.bin      pictures for the touch screen
    output/sdcard/cias/DKR.cia              the game, to install on the HOME Menu
    output/pictures/*.png                   the icon and the banner, to look at

The program code is already built (builder/prebuilt/); the ROM supplies the
game's data and the pictures. Needs Python 3.6 or newer and nothing else.
"""
import argparse
import hashlib
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import characters   # noqa: E402
import ctr          # noqa: E402
import picture      # noqa: E402
import rom          # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PREBUILT = os.path.join(HERE, 'prebuilt')
CHARACTERS = os.path.join(ROOT, 'characters', 'added-characters.pack')
ROM_ENDINGS = ('.z64', '.n64', '.v64', '.rom', '.zip')


def say(text=''):
    print(text)
    sys.stdout.flush()


def stop(text):
    say()
    say('STOPPED: ' + text)
    sys.exit(1)


def candidates():
    """ROM-like files in the places a ROM may have been put."""
    found = []
    for folder in (ROOT, os.path.join(ROOT, 'rom')):
        if os.path.isdir(folder):
            for name in sorted(os.listdir(folder)):
                if name.lower().endswith(ROM_ENDINGS) and os.path.isfile(os.path.join(folder, name)):
                    found.append(os.path.join(folder, name))
    return found


def find_rom(named):
    """(path, ROM) of the first file that is the right ROM."""
    if named:
        if not os.path.isfile(named):
            stop('there is no file "%s".' % named)
        try:
            return named, rom.load_rom(named)
        except (rom.RomError, OSError, ValueError) as problem:
            stop('"%s" cannot be used:\n  %s' % (os.path.basename(named), problem))
    files = candidates()
    if not files:
        stop('no ROM found.\n'
             '  Put your Diddy Kong Racing ROM (%s) in the folder "rom"\n'
             '  (or next to BUILD.cmd), or drop the file onto BUILD.cmd.\n'
             '  It may be a .z64, .n64 or .v64 file, or a .zip with one inside.' % rom.ROM_NAME)
    problems = []
    for path in files:
        try:
            return path, rom.load_rom(path)
        except (rom.RomError, OSError, ValueError) as problem:
            problems.append('  %s:\n    %s' % (os.path.basename(path), str(problem).replace('\n', '\n    ')))
    stop('none of the files found is the ROM the game needs.\n' + '\n'.join(problems))


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def added_characters(data, wanted):
    """The files of the added characters, {name: bytes}; none if they are not wanted or not there."""
    if not wanted:
        say('     without the added characters, as asked')
        return {}
    if not os.path.isfile(CHARACTERS):
        say('     (no characters/added-characters.pack: the game will have its own ten characters)')
        return {}
    try:
        with open(CHARACTERS, 'rb') as f:
            return characters.read(f.read(), data)
    except (characters.PackError, OSError, ValueError) as problem:
        stop('characters/added-characters.pack cannot be used: %s.\n'
             '  Get that file again, or run the builder with --no-characters for the game without them.' % problem)


def clear_characters(game):
    """Take away the added characters of an earlier run: the game must never
    find a list that does not belong to its assets.bin."""
    for name in ('characters.txt', 'voices.bin'):
        if os.path.isfile(os.path.join(game, name)):
            os.remove(os.path.join(game, name))
    shutil.rmtree(os.path.join(game, 'voices'), ignore_errors=True)


def read_prebuilt(name):
    path = os.path.join(PREBUILT, name)
    if not os.path.isfile(path):
        stop('builder/prebuilt/%s is missing. It comes with the release; if you built the game\n'
             '  from the source yourself, tools/make-prebuilt.sh makes it (see docs/BUILDING.md).' % name)
    with open(path, 'rb') as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser(description='Makes Diddy Kong Racing for the Nintendo 3DS from your own ROM.')
    parser.add_argument('rom', nargs='?', help='the ROM file (default: look in this folder and in rom/)')
    parser.add_argument('--out', default=os.path.join(ROOT, 'output'), help='where to write (default: output/)')
    parser.add_argument('--no-characters', action='store_true',
                        help='leave out the added characters (characters/added-characters.pack)')
    args = parser.parse_args()

    say('Diddy Kong Racing for the Nintendo 3DS - builder')
    say()
    program = read_prebuilt('DKR.3dsx')
    title = ctr.Cia(read_prebuilt('DKR.cia'))

    say('1/4  Checking the ROM')
    path, data = find_rom(args.rom)
    say('     %s' % os.path.basename(path))
    say('     is %s - good.' % rom.ROM_NAME)
    assets = rom.Assets(data)

    sd = os.path.join(args.out, 'sdcard')
    game = os.path.join(sd, '3ds', 'DKR')

    say('2/4  Taking the game\'s data out of the ROM')
    added = added_characters(data, not args.no_characters)
    clear_characters(game)
    game_data = added.get('assets.bin', assets.data)
    write(os.path.join(game, 'assets.bin'), game_data)
    write(os.path.join(game, 'assets.lut.bin'), added.get('assets.lut.bin', assets.directory))
    say('     assets.bin (%.1f MB), assets.lut.bin' % (len(game_data) / 1048576.0))
    if added:
        for name, contents in sorted(added.items()):
            if name not in ('assets.bin', 'assets.lut.bin'):
                write(os.path.join(game, *name.split('/')), contents)
        who = characters.names(added)
        say('     with %d added characters: %s' % (len(who), ', '.join(who)))

    say('3/4  Making the pictures from the ROM\'s own')
    icon = picture.icon(assets)
    banner = picture.banner(assets)
    write(os.path.join(args.out, 'pictures', 'icon.png'), icon.png())
    write(os.path.join(args.out, 'pictures', 'banner.png'), banner.png())
    for name, made in (('logo', picture.touch_logo(assets)), ('banana', picture.banana(assets)),
                       ('sky', picture.sky())):
        write(os.path.join(game, 'bottom', name + '.bin'), made.touch_screen_file())
    say('     icon, banner, and the touch screen\'s logo, banana and sky')

    say('4/4  Writing the game')
    files = title.program_files()
    smdh = ctr.smdh_with_icon(files['icon'], icon)
    cia = title.with_pictures(ctr.banner_with_picture(files['banner'], banner), smdh)
    faults = ctr.Cia(cia).faults()
    if faults:
        stop('the .cia did not come out right (%s). Please report this.' % ', '.join(faults))
    launcher_smdh_at = int.from_bytes(program[0x20:0x24], 'little')
    launcher = ctr.threedsx_with_icon(program, ctr.smdh_with_icon(
        program[launcher_smdh_at:launcher_smdh_at + ctr.SMDH_BYTES], icon))
    write(os.path.join(game, 'DKR.3dsx'), launcher)
    write(os.path.join(sd, 'cias', 'DKR.cia'), cia)
    say('     DKR.3dsx (%.1f MB), DKR.cia (%.1f MB)' % (len(launcher) / 1048576.0, len(cia) / 1048576.0))

    say()
    say('DONE. Everything is in:')
    say('  %s' % os.path.abspath(sd))
    say()
    say('What to do now:')
    say('  1. Copy the two folders inside it ("3ds" and "cias") onto the root of your')
    say('     console\'s SD card, merging with the folders already there.')
    say('  2. Start the game from the Homebrew Launcher (it is listed as DKR), or')
    say('     install cias/DKR.cia with FBI to have it on the HOME Menu.')
    say('  README.md has the details, the controls and what to do if it does not start.')
    say()
    say('  (SHA-1 of assets.bin: %s)' % hashlib.sha1(game_data).hexdigest())


if __name__ == '__main__':
    if sys.version_info < (3, 6):
        sys.exit('This needs Python 3.6 or newer; this is Python %d.%d.' % sys.version_info[:2])
    main()
