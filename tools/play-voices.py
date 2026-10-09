#!/usr/bin/env python3
"""Play every character's every voice line in the game and check what came out.

    python tools/play-voices.py make                 write the test scripts to out/play-voices/
    tools/play-voices.sh                             run them all in the emulator and check (about 25 minutes)
    python tools/play-voices.py check                check the recordings the runs left in out/play-voices/

check-voices.py and the VOICECHECK test command show that each recording is
in its own character's file and is read from there. This goes the rest of
the way: each line is played with the SOUND test command on the silent Game
Select screen (sound table, synthesizer, the audio thread reading the voice
file's samples, the mixer), the game records what it mixes
(DKR_AUDIODUMP=1), and the recording is compared with the line's own samples
decoded here. A line passes if the game's output is that recording: the wave
forms agree (correlation of at least PASS, 1.0 being identical; an unrelated
recording gives about 0.1). Each is also compared with the line that follows
it in the script: unless the two are the same recording, that must not
match (the "next line" column of out/play-voices/NN.report.txt).

The ten characters of the game are played the same way as the added ones.
"""
import argparse
import importlib.util
import json
import math
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'out', 'play-voices')
RATE = 22050            # what the game mixes
FPS = 60
FIRST_FRAME = 2400      # on the quiet screen native/tests/sounds.txt uses, the music silenced
GAP = 0.9               # seconds of silence between two lines
PER_SCRIPT = 110        # 3ds/autotest.c holds 128 commands
PASS = 0.80
SILENCE = 200           # a peak below this is no sound at all


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), os.path.join(ROOT, 'tools', name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def lines_of_pack(pack):
    """[(character, line, sound id)] for the ten and the added characters, and the pack's sounds."""
    imp = load('import-characters')
    assets, sounds, rows = imp.read_pack(pack)
    result = []
    for character, name in enumerate(imp.CHARACTERS):
        for sound_id, line in sorted(imp.voice_lines(character).items()):
            if sounds.recording(sound_id) is not None:
                result.append((name, line, sound_id))
    for name, donor, ids, mapping, voice_file in rows:
        own, seen = imp.voice_lines(donor), set()
        for game_id, to in sorted(mapping.items()):
            if to and to not in seen:
                seen.add(to)
                result.append((name, own[game_id], to))
    return result, sounds


def expected(sounds, sound_id, decode):
    """A line as the game should play it: its samples at the pitch the game gives them, at the mixer's rate."""
    recording = sounds.recording(sound_id)
    samples = np.array(decode(recording), dtype=np.float64)
    # The sound table's pitch times the key map's (src/audiosfx.c: keyBase in
    # semitones about 60, detune in cents); the voices are keyBase 48, half speed.
    key_base, detune = recording['keymap'][4], recording['keymap'][5]
    detune -= 256 if detune > 127 else 0
    pitch = (recording['row'][2] or 100) / 100.0 * 2.0 ** ((key_base * 100 - 6000 + detune) / 1200.0)
    count = int(len(samples) / pitch)
    # The sound's envelope ends it after its attack and decay times
    # (microseconds, at pitch 1), however long the recording is: some of the
    # game's own lines are cut short by theirs, on the N64 as here.
    attack, decay = struct.unpack('>ii', recording['envelope'][:8])
    if decay >= 0:
        count = min(count, int((max(attack, 0) + decay) / pitch * RATE / 1e6))
    return np.interp(np.arange(count) * pitch, np.arange(len(samples)), samples)


def make(args):
    lines, sounds = lines_of_pack(args.pack)
    decode = load('check-voices').decode
    os.makedirs(OUT, exist_ok=True)
    manifest = []
    for number, start in enumerate(range(0, len(lines), PER_SCRIPT), 1):
        frame, entries = FIRST_FRAME, []
        text = ['# Written by tools/play-voices.py: voice lines played one by one on the Game Select',
                '# screen, to record (DKR_AUDIODUMP=1 DKR_CHARACTERS=out/characters).',
                '1000 8 START', '1100 8 START', '1250 8 A', '1400 8 A', '1550 8 A', '1800 8 A', '2000 8 A',
                '2200 NOMUSIC']
        for name, line, sound_id in lines[start:start + PER_SCRIPT]:
            seconds = len(expected(sounds, sound_id, decode)) / RATE
            text.append('%d SOUND %d' % (frame, sound_id))
            entries.append({'character': name, 'line': line, 'sound': sound_id, 'frame': frame, 'seconds': seconds})
            frame += int(math.ceil((seconds + GAP) * FPS))
        text.append('%d EXIT' % frame)
        with open(os.path.join(OUT, '%02d.txt' % number), 'w', newline='\n') as f:
            f.write('\n'.join(text) + '\n')
        manifest.append({'script': '%02d.txt' % number, 'frames': frame, 'lines': entries})
    with open(os.path.join(OUT, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print('%d lines of %d characters in %d scripts in %s (%.0f minutes of play)' % (
        len(lines), len(set(line[0] for line in lines)), len(manifest), OUT,
        sum(entry['frames'] for entry in manifest) / FPS / 60.0))


def match(heard, wanted):
    """How well `wanted` is found in `heard`: (correlation at the best place, the place)."""
    if len(wanted) < 64 or len(heard) < len(wanted):
        return 0.0, 0
    size = 1 << int(math.ceil(math.log2(len(heard) + len(wanted))))
    product = np.fft.irfft(np.fft.rfft(heard, size) * np.conj(np.fft.rfft(wanted, size)), size)[:len(heard) - len(wanted) + 1]
    # The energy of `heard` under the window at each place.
    total = np.concatenate(([0.0], np.cumsum(heard * heard)))
    under = total[len(wanted):len(wanted) + len(product)] - total[:len(product)]
    norm = np.sqrt(np.maximum(under, 1.0) * max(float(np.dot(wanted, wanted)), 1.0))
    score = product / norm
    place = int(np.argmax(score))
    # The first place as good as the best: two lines one after the other may
    # be the same recording, and the earlier is the one that was asked for.
    place = int(np.argmax(score >= score[place] - 0.02))
    return float(score[place]), place


def check(args):
    with open(os.path.join(OUT, 'manifest.json')) as f:
        manifest = json.load(f)
    lines, sounds = lines_of_pack(args.pack)
    decode = load('check-voices').decode
    failures, done, worst, characters = [], 0, 1.0, {}
    for script in manifest:
        stem = os.path.splitext(script['script'])[0]
        path = os.path.join(OUT, stem + '.raw')
        if not os.path.isfile(path):
            failures.append('%s: no recording (%s)' % (script['script'], path))
            continue
        stereo = np.fromfile(path, dtype='<i2').astype(np.float64)
        heard = (stereo[0::2][:len(stereo) // 2] + stereo[1::2][:len(stereo) // 2]) / 2
        entries = script['lines']
        clips = [expected(sounds, entry['sound'], decode) for entry in entries]
        report, last = [], 0
        for i, entry in enumerate(entries):
            # Where to listen: from the end of the line before (the emulator
            # does not keep 60 frames a second, so the script's frames only
            # say roughly when) to a little after this one should be over.
            begin = max(last, int((entry['frame'] / FPS - 0.5) * RATE))
            end = min(len(heard), begin + len(clips[i]) + int((GAP + 2.5) * RATE))
            window = heard[begin:end]
            peak = float(np.max(np.abs(window))) if len(window) else 0.0
            score, place = match(window, clips[i])
            other = clips[(i + 1) % len(clips)]
            own = window[place:place + len(clips[i])]     # the stretch this line was found in
            wrong = match(np.concatenate((own, np.zeros(max(0, len(other) - len(own))))), other)[0]
            what = '%s %s (sound %d)' % (entry['character'], entry['line'], entry['sound'])
            tally = characters.setdefault(entry['character'], [0, 0, 1.0])
            tally[0] += 1
            done += 1
            if len(window) < len(clips[i]):
                failures.append('%s: the recording ends before it' % what)
            elif peak < SILENCE:
                failures.append('%s: silence' % what)
            elif score < PASS:
                failures.append('%s: something sounds, but not this recording (%.2f)' % (what, score))
            else:
                last = begin + place + len(clips[i])
                tally[1] += 1
                tally[2] = min(tally[2], score)
                worst = min(worst, score)
            report.append('%-14s %-10s sound %4d  at %6.2f s  peak %5d  match %.2f  next line %.2f' % (
                entry['character'], entry['line'], entry['sound'], (begin + place) / RATE, peak, score, wrong))
        with open(os.path.join(OUT, stem + '.report.txt'), 'w') as f:
            f.write('\n'.join(report) + '\n')
    print('%-15s %s' % ('character', 'lines heard as its own recordings'))
    for name, (count, good, low) in characters.items():
        print('%-15s %2d of %2d%s' % (name, good, count, '   (weakest match %.2f)' % low if good else ''))
    print('\n%d lines played, %d heard as their own recordings (weakest match %.2f; %d wanted)' % (
        done, done - sum(1 for f in failures if 'no recording' not in f), worst, len(lines)))
    if failures or done != len(lines):
        print('\nNOT RIGHT (%d):' % len(failures))
        for failure in failures[:80]:
            print('  ' + failure)
        sys.exit(1)
    print('every line of every character plays, as its own recording')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('what', choices=('make', 'check'))
    p.add_argument('--pack', default=os.path.join(ROOT, 'out', 'characters'))
    args = p.parse_args()
    (make if args.what == 'make' else check)(args)


if __name__ == '__main__':
    main()
