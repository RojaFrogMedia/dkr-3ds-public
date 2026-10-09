#!/usr/bin/env python3
"""Check that every character has its own voice, and say who says what.

    python tools/check-voices.py                         the pack in out/characters
    python tools/check-voices.py --wav out/voices-wav    ... and write every line as a .wav to listen to
    python tools/check-voices.py --log out/native-run/log.txt
                                                         ... and compare with what the game itself read
                                                         (the VOICECHECK command of a test script)

What is checked (check_pack in tools/import-characters.py):
  - the game's own 640 sounds, among them the ten characters' voices, are
    byte for byte the ROM's;
  - every recording of an added character is for one of its own lines, is in
    its own file voices/<character>.bin, where that file's list says, and
    belongs to no other character;
  - for every character whose patch is in the patches folder: each recording
    is the one the patch has for that line.

The table it prints is what the game does for each character (modchar_sound
in native/dkr-pc/3ds/characters.c): its own recording; for a cheer or groan
the patch has none of, its own nearest one; for a character select line the
patch has none of, nothing; and the donor's horn if the patch brought none.

--wav writes <folder>/<character>/<line>.wav for the ten and the added
characters alike, one folder per character, at the pitch the game plays
them (the sound table's and the key map's; tools/play-voices.py checks that
against what the game really puts out).
"""
import argparse
import importlib.util
import os
import re
import struct
import sys
import wave
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
spec = importlib.util.spec_from_file_location('import_characters', os.path.join(ROOT, 'tools', 'import-characters.py'))
imp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imp)


def what_plays(donor, mapping):
    """{line: what the game plays for it} for an added character, by the game's rules."""
    result = {}
    for game_id, line in sorted(imp.voice_lines(donor).items(), key=lambda item: item[0]):
        if game_id in mapping:
            result[line] = 'own' if mapping[game_id] else 'silent (the patch made it so)'
        elif line == 'horn':
            result[line] = "%s's horn" % imp.CHARACTERS[donor]
        elif line.startswith(('positive', 'negative')):
            kind, row = line[:8], int(line[8:])
            own = [int(other[8:]) for other_id, other in imp.voice_lines(donor).items()
                   if other.startswith(kind) and mapping.get(other_id)]
            result[line] = ('its own %s%d' % (kind, min(own, key=lambda r: (abs(r - row), r)))) if own else 'silent'
        else:
            result[line] = 'silent'
    return result


def decode(recording):
    """A recording's samples as 16-bit numbers."""
    data = recording['samples']
    if recording['kind'] == imp.Sounds.RAW16:
        return list(struct.unpack('>%dh' % (len(data) // 2), data[:len(data) & ~1]))
    # The N64's ADPCM: frames of 9 bytes for 16 samples, predicted from the
    # samples before by a code book.
    book = recording['book']
    order, predictors = imp.u32(book, 0), imp.u32(book, 4)
    raw = struct.unpack('>%dh' % (order * predictors * 8), book[8:8 + order * predictors * 16])
    table = []
    for p in range(predictors):
        rows = [[0] * (order + 8) for _ in range(8)]
        for j in range(order):
            for k in range(8):
                rows[k][j] = raw[(p * order + j) * 8 + k]
        for k in range(1, 8):
            rows[k][order] = rows[k - 1][order - 1]
        rows[0][order] = 1 << 11
        for k in range(1, 8):
            for j in range(k, 8):
                rows[j][k + order] = rows[j - k][order]
        table.append(rows)
    out, state = [], [0] * 16
    for at in range(0, len(data) - 8, 9):
        scale, rows = 1 << (data[at] >> 4), table[(data[at] & 15) % predictors]
        ix = []
        for byte in data[at + 1:at + 9]:
            for nibble in (byte >> 4, byte & 15):
                ix.append((nibble - 16 if nibble & 8 else nibble) * scale)
        for half in range(2):
            vector = (state[16 - order:] if half == 0 else state[8 - order:8]) + ix[half * 8:half * 8 + 8]
            for i in range(8):
                state[half * 8 + i] = sum(a * b for a, b in zip(rows[i], vector)) >> 11
        out += [max(-32768, min(32767, v)) for v in state]
    return out


def write_wav(path, recording):
    samples = decode(recording)
    with wave.open(path, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        # The rate the game plays it at: the sound table's pitch (100 = as
        # recorded) times the key map's (keyBase 60 = as recorded, 48 = half).
        key_base, detune = recording['keymap'][4], recording['keymap'][5]
        detune -= 256 if detune > 127 else 0
        f.setframerate(max(4000, int(22050 * (recording['row'][2] or 100) / 100.0 *
                                     2.0 ** ((key_base * 100 - 6000 + detune) / 1200.0))))
        f.writeframes(struct.pack('<%dh' % len(samples), *samples))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--pack', default=os.path.join(ROOT, 'out', 'characters'))
    p.add_argument('--rom', default=imp.DEFAULT_ROM)
    p.add_argument('--patches', default=imp.DEFAULT_PATCHES, help='folder of .xdelta files to compare with ("-": none)')
    p.add_argument('--wav', help='folder to write every line to, as .wav, a folder per character')
    p.add_argument('--log', help="the game's log of a run with the VOICECHECK test command")
    args = p.parse_args()

    importer = imp.Importer(imp.read_rom(args.rom))
    try:
        assets, sounds, rows = imp.read_pack(args.pack)
    except (OSError, ValueError) as error:
        sys.exit('%s' % error)
    in_pack = dict((row[0], row) for row in rows)
    expected, patch_of = {}, {}
    if args.patches != '-' and os.path.isdir(args.patches):
        for filename, name, donor, patched in imp.patch_characters(importer, args.patches, imp.read_names(args.patches),
                                                                   report=lambda text: None):
            if name in in_pack and in_pack[name][1] == donor:
                expected[name], others = importer.character_lines(imp.Sounds(patched), donor)
                patch_of[name] = filename
    problems, looked = imp.check_pack(args.pack, importer.base, expected)

    print('%-15s %-8s %-22s %s' % ('character', 'voice of', 'voice file', 'lines'))
    for name, voice in zip(imp.CHARACTERS, range(10)):
        print('%-15s %-8s %-22s %s' % (name, 'the game', 'assets.bin', 'all 19 its own'))
    for name, donor, ids, mapping, voice_file in rows:
        plays = what_plays(donor, mapping)
        others = ['%s: %s' % (line, what) for line, what in plays.items() if what != 'own']
        print('%-15s %-8s %-22s %d its own%s' % (name, 'patch' if name in patch_of else 'pack', 'voices/' + voice_file,
                                                 sum(1 for what in plays.values() if what == 'own'),
                                                 ''.join('; ' + other for other in others)))
    without = [row[0] for row in rows if row[0] not in patch_of]
    print("\n%d characters; %d recordings, each in its own character's file; %d compared with the patches; "
          "the game's own %d sounds compared with the ROM" % (looked['characters'], looked['recordings'],
                                                              looked['compared'], looked['own']))
    if without:
        print('no patch at hand to compare with for: %s (their recordings are checked for place and owner only)'
              % ', '.join(without))

    if args.log:
        # What the game read, character by character: the number of
        # recordings and a check sum over their samples as the audio thread
        # is given them (modchar_voices_check in 3ds/characters.c).
        seen = {}
        for line in open(args.log, encoding='utf-8', errors='replace'):
            match = re.match(r'VOICECHECK: ([^|]+)\|(\d+)\|(\d+)\|(\d+)\|([0-9a-f]{8})', line)
            if match:
                seen[match.group(1)] = tuple(int(v) for v in match.group(2, 3, 4)) + (match.group(5),)
        if not seen:
            problems.append('%s has no VOICECHECK lines' % args.log)
        for number, (name, donor, ids, mapping, voice_file) in enumerate(rows, 1):
            crc, good, silent = 0, 0, 0
            for game_id, to in sorted(mapping.items()):
                if to == 0:
                    silent += 1
                    continue
                owner, offset, length = sounds.place(to)
                crc = zlib.crc32(sounds.voice_files[number][offset:offset + length], crc)
                good += 1
            want = (good, silent, 0, '%08x' % (crc & 0xffffffff))
            if seen and seen.get(name[:15]) != want:
                problems.append('%s: the game read %s, the pack has %s' % (name, seen.get(name[:15]), want))
        if seen and not any('the game read' in problem for problem in problems):
            print('the game read the same: %d characters, every recording from its own file (%s)' % (len(seen), args.log))

    if args.wav:
        count = 0
        for character, name in enumerate(imp.CHARACTERS):
            folder = os.path.join(args.wav, name)
            os.makedirs(folder, exist_ok=True)
            for game_id, line in imp.voice_lines(character).items():
                recording = importer.base_sounds.recording(game_id)
                if recording is not None:
                    write_wav(os.path.join(folder, line + '.wav'), recording)
                    count += 1
        for name, donor, ids, mapping, voice_file in rows:
            folder = os.path.join(args.wav, name)
            os.makedirs(folder, exist_ok=True)
            own = imp.voice_lines(donor)
            for game_id, to in mapping.items():
                if to:
                    write_wav(os.path.join(folder, own[game_id] + '.wav'), sounds.recording(to))
                    count += 1
        print('%d lines written to %s, a folder per character' % (count, args.wav))

    if problems:
        print('\nNOT RIGHT (%d):' % len(problems))
        for problem in problems[:60]:
            print('  ' + problem)
        sys.exit(1)
    print('all voices are with their own characters')


if __name__ == '__main__':
    main()
