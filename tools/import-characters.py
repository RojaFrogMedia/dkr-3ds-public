#!/usr/bin/env python3
"""Turn character patches (.xdelta) into extra characters for the 3DS game.

    python tools/import-characters.py
    python tools/import-characters.py --rom rom/dkr.z64 --patches characters --out out/characters

A character patch is made to REPLACE one of the game's ten racers (the
"donor"). This tool applies each patch to the ROM in memory, finds which
racer it replaced, and copies that racer's models, textures, animations,
character select model and portrait out of the patched ROM into the game's
asset file as NEW entries, next to the originals. So every patch becomes an
additional character and the ten originals stay as they are.

It writes these, which go in sdmc:/3ds/DKR/ on the SD card:

    assets.bin, assets.lut.bin   the game's assets with the characters added
    voices/<character>.bin       each character's own voice recordings, one
                                 file per character (layout: class Sounds)
    characters.txt               the list the game reads (3ds/characters.c)

and, to look at, portraits.png. Nothing of a patch's program code is used:
an added character drives like its donor and has the donor's engine sound;
its voice is the patch's, and so is its horn if the patch has one.

A character is given only the recordings of its own lines (voice_lines
below), and they are kept apart from every other character's: the game
plays an added character's line from that character's file or not at all.
When the files are written they are read back and compared, line by line,
with what was meant to go in, and the game's own sounds with the ROM's;
tools/check-voices.py does the same for a pack at any time.

--base-characters adds patches to an existing pack (for characters whose
patch files are no longer at hand): its characters are kept, and the sound
bank and voice files are written afresh for all of them.

names.txt in the patches folder (optional) names characters or leaves them
out, one per line:

    mario.xdelta = Mario
    DKR-Yoshi-Racing-Story64v04.xdelta#Timber = Yoshi     (a patch with two)
    Native Diddy Test.xdelta = skip

How the asset file is laid out is at the top of the code below.
"""
import argparse
import array
import hashlib
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcdiff

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ROM = os.path.join(ROOT, 'rom', 'Diddy Kong Racing (USA) (En,Fr).z64')
DEFAULT_PATCHES = os.path.join(ROOT, 'characters')

# --- The asset file -------------------------------------------------------
# A directory ("LUT": a count of 50, then 51 offsets) followed by 50 sections.
# The sections used here come in pairs, a table of offsets and the records:
TEX3D, TEX3D_TABLE = 2, 3           # textures of 3D models
TEX2D, TEX2D_TABLE = 4, 5           # textures of the menus and HUD (portraits)
MODEL_TABLE, MODELS = 28, 29        # object models, compressed
MODEL_ANIMS = 30                    # per model: its first animation (u16); the next entry ends it
ANIM_TABLE, ANIMS = 31, 32          # animations
HEADER_TABLE, HEADERS = 33, 34      # object headers ("what an object is")
OBJECT_IDS = 35                     # 512 x u16: object id -> header; 512 = unused
AUDIO_TABLE, AUDIO = 38, 39         # all the sound; see class Sounds
BASE_LUT = 0xecb60                  # where the directory is in the US 1.0 ROM
LUT_BYTES = 208

# The game's character numbers, and for each: its racer objects (car, hover,
# plane - gRacerObjectTable in src/objects.c), the header of its character
# select model, and its portrait among the 2D textures.
CHARACTERS = ('Krunch', 'Bumper', 'Tiptup', 'Conker', 'Timber', 'Banjo', 'Drumstick', 'Pipsy', 'T.T.', 'Diddy')
RACER_OBJECTS = ((1, 55, 66), (2, 56, 67), (3, 57, 68), (4, 58, 69), (81, 87, 91),
                 (84, 88, 92), (85, 89, 93), (86, 90, 94), (237, 243, 244), (255, 253, 257))
SELECT_HEADERS = (169, 171, 172, 170, 173, 175, 176, 177, 40, 174)
PORTRAITS = (122, 123, 124, 126, 125, 127, 128, 129, 130, 131)
UNUSED_OBJECT = 512
SOUNDS_OF_THE_GAME = 640            # sound ids of the game's own

# The sound ids of a character's own voice lines: one id per character for
# the two lines of the character select and the horn, and 8 cheers and 8
# groans in rows of 12 (ten characters and two never used). The same numbers
# are in native/dkr-pc/include/sound_ids.h and sound_voice_line (src/audio.c).
SOUND_SELECT, SOUND_DESELECT, SOUND_HORN, SOUND_POSITIVE, SOUND_NEGATIVE = 0x87, 0x93, 0x156, 0x162, 0x1c2
VOICE_ROWS, VOICE_ROW_STEP = 8, 12


def voice_lines(character):
    """{sound id: what the line is} for one of the game's ten characters."""
    lines = {SOUND_SELECT + character: 'select', SOUND_DESELECT + character: 'deselect',
             SOUND_HORN + character: 'horn'}
    for row in range(VOICE_ROWS):
        lines[SOUND_POSITIVE + character + VOICE_ROW_STEP * row] = 'positive%d' % (row + 1)
        lines[SOUND_NEGATIVE + character + VOICE_ROW_STEP * row] = 'negative%d' % (row + 1)
    return lines


def file_name(name, taken):
    """A plain file name for a character's voice file: 'K. Rool' -> 'k-rool.bin'."""
    stem = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-') or 'character'
    result, number = stem + '.bin', 2
    while result in taken:
        result, number = '%s-%d.bin' % (stem, number), number + 1
    taken.add(result)
    return result


def read_rom(path):
    with open(path, 'rb') as f:
        rom = f.read()
    if hashlib.sha1(rom).hexdigest() != '0cb115d8716dbbc2922fda38e533b9fe63bb9670':
        sys.exit('the ROM is not Diddy Kong Racing US 1.0 (.z64)')
    return rom
BANANAS_PER_CHARACTER = 150         # also in 3ds/characters.c


def u16(d, at):
    return struct.unpack_from('>H', d, at)[0]


def u32(d, at):
    return struct.unpack_from('>I', d, at)[0]


def pad16(data):
    return bytes(data) + bytes(-len(data) % 16)


class Assets:
    """The 50 sections of an asset file, with helpers to read and add records."""

    def __init__(self, rom, lut):
        if u32(rom, lut) != 50:
            raise ValueError('no asset directory at %#x' % lut)
        offsets = [u32(rom, lut + 4 + 4 * i) for i in range(51)]
        start = lut + LUT_BYTES
        self.sections = [bytearray(rom[start + a:start + b]) for a, b in zip(offsets, offsets[1:])]

    @classmethod
    def from_pack(cls, folder):
        with open(os.path.join(folder, 'assets.bin'), 'rb') as f:
            data = f.read()
        with open(os.path.join(folder, 'assets.lut.bin'), 'rb') as f:
            lut = f.read()
        if len(lut) != LUT_BYTES or u32(lut, 0) != 50:
            raise ValueError('invalid generated asset pack')
        offsets = [u32(lut, 4 + 4 * i) for i in range(51)]
        result = cls.__new__(cls)
        result.sections = [bytearray(data[a:b]) for a, b in zip(offsets, offsets[1:])]
        return result

    def offsets(self, table):
        data, out = self.sections[table], []
        for at in range(0, len(data), 4):
            if u32(data, at) == 0xffffffff:
                return out
            out.append(u32(data, at))
        raise ValueError('table %d has no end mark' % table)

    def count(self, table):
        return len(self.offsets(table)) - 1

    def record(self, table, section, index):
        offsets = self.offsets(table)
        return bytes(self.sections[section][offsets[index]:offsets[index + 1]])

    def add(self, table, section, data):
        """Append a record; returns its index."""
        offsets = self.offsets(table)
        if offsets[-1] != len(self.sections[section]):
            raise ValueError('section %d does not end where its table says' % section)
        self.sections[section] += pad16(data)
        offsets.append(len(self.sections[section]))
        self.sections[table] = bytearray(pad16(struct.pack('>%dI' % len(offsets), *offsets) + b'\xff' * 4))
        return len(offsets) - 2

    def write(self, folder):
        lut, at = struct.pack('>I', 50), 0
        for section in self.sections:
            lut += struct.pack('>I', at)
            at += len(section)
        lut += struct.pack('>I', at)
        with open(os.path.join(folder, 'assets.lut.bin'), 'wb') as f:
            f.write(lut)
        with open(os.path.join(folder, 'assets.bin'), 'wb') as f:
            for section in self.sections:
                f.write(section)
        return at


def find_lut(rom):
    """Patches made from a rebuilt ROM have the directory somewhere else."""
    at = rom.find(b'\x00\x00\x00\x32\x00\x00\x00\x00')
    while at >= 0:
        offsets = [u32(rom, at + 4 + 4 * i) for i in range(51)]
        if all(a <= b for a, b in zip(offsets, offsets[1:])) and offsets[-1] > 0x100000:
            return at
        at = rom.find(b'\x00\x00\x00\x32\x00\x00\x00\x00', at + 4)
    raise ValueError('no asset directory in the patched ROM')


def unpack(record):
    """A compressed record: size (4 bytes, little-endian), a level byte, raw deflate."""
    size = int.from_bytes(record[:4], 'little')
    data = zlib.decompressobj(-15).decompress(record[5:], size + 1)
    if len(data) != size:
        raise ValueError('compressed record is damaged')
    return data


def pack(data):
    deflate = zlib.compressobj(9, zlib.DEFLATED, -15)
    return len(data).to_bytes(4, 'little') + b'\x09' + deflate.compress(bytes(data)) + deflate.flush()


def swap16(samples):
    """16-bit samples the other way round (an odd last byte stays)."""
    even = len(samples) & ~1
    words = array.array('H', bytes(samples[:even]))
    words.byteswap()
    return words.tobytes() + bytes(samples[even:])


class Sounds:
    """The sound effects of an asset file.

    The audio section is a run of parts, whose offsets the audio table
    lists. Three matter here:
      part 1   the sound bank ("ctl"): for every recording an ALSound, which
               points at an envelope, a key map and a wave table; the wave
               table says where the samples are and how they are packed
      part 2   the samples ("tbl")
      part 6   the sound table: 10 bytes per sound id, the first two the
               number of the recording it plays (from 1)
    This reads them, and for the game's own file adds new sounds: the bank
    and the sound table grow; the samples go to files of their own, one per
    added character (voices/<character>.bin), because the game holds the
    whole asset file in memory.

    A wave table says where its samples are as an offset from the samples'
    part. For an added character's recording that offset is made to come out,
    as a place in the asset file, at
        VOICE_BASE + (the character's number << 24) + the place in its file
    (pc_voice_offset in native/dkr-pc/3ds/reimpl.c takes it apart again). It
    depends on where the samples' part is, which moves whenever anything
    before it in the asset file grows, so finish() writes every such offset
    again each time; a pack is never added to by leaving old ones in place.

    A voice file: 'DKRV', version 1, the number of recordings, the
    character's name (16 bytes); then per recording the sound id that plays
    it, where it is in this file and how long (three 32-bit numbers, low byte
    first); then the samples. The game checks each recording it is about to
    play against this list (voice_want in 3ds/characters.c).
    """
    VOICE_BASE = 0x40000000
    SLOT_SHIFT, SLOT_BYTES, SLOTS = 24, 1 << 24, 32
    ADPCM, RAW16 = 0, 1
    HEADER, ENTRY, VERSION = '<4sII16s', '<III', 1

    def __init__(self, assets, voice_files=None):
        raw = assets.sections[AUDIO_TABLE]
        self.table = [u32(raw, 4 * i) for i in range(13)]
        audio = assets.sections[AUDIO]
        self.parts = [bytes(audio[:self.table[0]])]
        self.parts += [bytes(audio[a:b]) for a, b in zip(self.table, self.table[1:])]
        self.tail = bytes(audio[self.table[12]:])
        self.ctl = bytearray(self.parts[2])
        self.tbl = self.parts[3]
        self.rows = bytearray(self.parts[7])
        self.instrument = u32(self.ctl, u32(self.ctl, 4) + 12)     # bank 0's instrument 0
        count = u16(self.ctl, self.instrument + 14)
        self.recordings = [u32(self.ctl, self.instrument + 16 + 4 * i) for i in range(count)]
        # Where the samples' part begins in this asset file.
        self.samples_in_file = sum(len(section) for section in assets.sections[:AUDIO]) + self.table[2]
        self.voice_files = voice_files or {}    # reading a pack: {character number: its voice file}
        self.voices = {}            # adding: {character number: its samples so far}
        self.entries = {}           # {character number: [(sound id, offset among its samples, bytes)]}
        self.fixups = []            # (wave table offset in ctl, character number, offset among its samples)
        self.added = {}             # (character number, recording) -> new sound id, so that none is added twice

    def sound_count(self):
        return len(self.rows) // 10

    def place(self, sound_id):
        """Where a sound id's samples are: (character number, offset in its voice file, bytes),
        or (None, offset in the samples' part, bytes) for the game's own. None: it plays nothing."""
        number = u16(self.rows, 10 * sound_id)
        if not 0 < number <= len(self.recordings):
            return None
        wave = u32(self.ctl, self.recordings[number - 1] + 8)
        base, length = u32(self.ctl, wave), u32(self.ctl, wave + 4)
        in_file = (base + self.samples_in_file) & 0xffffffff
        if in_file < self.VOICE_BASE:
            return None, base, length
        in_file -= self.VOICE_BASE
        return in_file >> self.SLOT_SHIFT, in_file & (self.SLOT_BYTES - 1), length

    def recording(self, sound_id):
        """Everything a sound id plays, as a dict of byte strings (None: silence)."""
        place = self.place(sound_id)
        if place is None:
            return None
        owner, offset, length = place
        ctl, at = self.ctl, self.recordings[u16(self.rows, 10 * sound_id) - 1]
        envelope, keymap, wave = u32(ctl, at), u32(ctl, at + 4), u32(ctl, at + 8)
        kind = ctl[wave + 8]
        loop = u32(ctl, wave + 12)
        if owner is None:
            samples = self.tbl[offset:offset + length]
        else:
            data = self.voice_files.get(owner, b'')
            if offset + length > len(data):
                raise ValueError("sound %d's samples are not in the voice file of character %d" % (sound_id, owner))
            samples = data[offset:offset + length]
            if kind == self.RAW16:      # stored the 3DS's way round; see add()
                samples = swap16(samples)
        r = {'row': bytes(self.rows[10 * sound_id + 2:10 * sound_id + 10]), 'sound': bytes(ctl[at + 12:at + 16]),
             'envelope': bytes(ctl[envelope:envelope + 16]), 'keymap': bytes(ctl[keymap:keymap + 6]),
             'kind': kind, 'flags': ctl[wave + 9], 'samples': bytes(samples), 'book': b'', 'loop': b''}
        if kind == self.ADPCM:
            book = u32(ctl, wave + 16)
            r['book'] = bytes(ctl[book:book + 8 + u32(ctl, book) * u32(ctl, book + 4) * 16])
            r['loop'] = bytes(ctl[loop:loop + 44]) if loop else b''
        else:
            r['loop'] = bytes(ctl[loop:loop + 12]) if loop else b''
        return r

    def put(self, data):
        """Append a structure to the bank; returns its offset."""
        self.ctl += bytes(-len(self.ctl) % 8)
        self.ctl += data
        return len(self.ctl) - len(data)

    def add(self, r, owner):
        """Add a recording (as recording() gives it) as a new sound id of character number `owner`."""
        key = (owner,) + tuple(sorted(r.items()))
        if key in self.added:
            return self.added[key]
        if not 0 < owner < self.SLOTS:
            raise ValueError('character number %d: there is room for %d' % (owner, self.SLOTS - 1))
        samples = r['samples']
        if r['kind'] == self.RAW16:     # the game wants these the 3DS's way round
            samples = swap16(samples)
        mine = self.voices.setdefault(owner, bytearray())
        where = len(mine)
        mine += samples + bytes(-len(samples) % 16)
        envelope, keymap = self.put(r['envelope']), self.put(r['keymap'] + bytes(2))
        loop = self.put(r['loop']) if r['loop'] else 0
        book = self.put(r['book']) if r['book'] else 0
        wave = self.put(struct.pack('>IIBBHII', 0, len(r['samples']), r['kind'], r['flags'], 0, loop, book))
        self.fixups.append((wave, owner, where))
        self.recordings.append(self.put(struct.pack('>III', envelope, keymap, wave) + r['sound']))
        self.rows += struct.pack('>H', len(self.recordings)) + r['row']
        self.added[key] = self.sound_count() - 1
        self.entries.setdefault(owner, []).append((self.added[key], where, len(r['samples'])))
        return self.added[key]

    def finish(self, assets, names):
        """Write the grown bank and sound table back into `assets`.

        Returns the voice files, {character number: bytes}; names[number - 1]
        is the name that goes in a file's head.
        """
        # The instrument lists its recordings after its 16-byte head, so it moves to the end.
        head = bytes(self.ctl[self.instrument:self.instrument + 14]) + struct.pack('>H', len(self.recordings))
        moved = self.put(head + struct.pack('>%dI' % len(self.recordings), *self.recordings))
        struct.pack_into('>I', self.ctl, u32(self.ctl, 4) + 12, moved)
        self.parts[2] = pad16(self.ctl)
        self.parts[7] = bytes(self.rows) + bytes(-len(self.rows) % 4)
        table, at = [], len(self.parts[0])
        for part in self.parts[1:]:
            table.append(at)
            at += len(part)
        table.append(at)
        # Each character's file: the head and the list, then its samples.
        files, heads = {}, {}
        for owner, entries in self.entries.items():
            heads[owner] = len(pad16(bytes(struct.calcsize(self.HEADER) + struct.calcsize(self.ENTRY) * len(entries))))
            head = struct.pack(self.HEADER, b'DKRV', self.VERSION, len(entries), names[owner - 1].encode('utf-8')[:15])
            for sound_id, where, length in entries:
                head += struct.pack(self.ENTRY, sound_id, heads[owner] + where, length)
            files[owner] = pad16(head) + bytes(self.voices[owner])
            if len(files[owner]) > self.SLOT_BYTES:
                raise ValueError("%s's voice is over %d MB" % (names[owner - 1], self.SLOT_BYTES >> 20))
        # A wave table's sample offset counts from the samples' part. The game
        # turns it into an offset in the asset file; see the class comment
        # for what that has to come out as.
        samples_in_file = sum(len(section) for section in assets.sections[:AUDIO]) + table[2]
        ctl = bytearray(self.parts[2])
        for wave, owner, where in self.fixups:
            place = self.VOICE_BASE + (owner << self.SLOT_SHIFT) + heads[owner] + where
            struct.pack_into('>I', ctl, wave, (place - samples_in_file) & 0xffffffff)
        self.parts[2] = bytes(ctl)
        assets.sections[AUDIO] = bytearray(b''.join(self.parts) + self.tail)
        struct.pack_into('>13I', assets.sections[AUDIO_TABLE], 0, *table)
        return files


def read_pack(folder):
    """A pack this tool wrote: its assets, its sounds, and its characters as
    (name, donor, [car, hover, plane, select, portrait], {the game's sound id: its own, 0 for silence}, voice file)."""
    assets = Assets.from_pack(folder)
    rows, files = [], {}
    with open(os.path.join(folder, 'characters.txt'), encoding='utf-8') as f:
        for line in f:
            fields = line.rstrip('\r\n').split('|')
            if line.startswith('#') or len(fields) < 2:
                continue
            if len(fields) < 9:
                raise ValueError('%s is a pack of the old kind (all voices in one voices.bin); it cannot be read'
                                 % folder)
            mapping = dict((int(a), int(b)) for a, b in (pair.split(':') for pair in fields[7].split(',') if ':' in pair))
            rows.append((fields[0], int(fields[1]), [int(value) for value in fields[2:7]], mapping, fields[8]))
    for number, row in enumerate(rows, 1):
        if row[4] != '-':
            with open(os.path.join(folder, 'voices', row[4]), 'rb') as f:
                files[number] = f.read()
    return assets, Sounds(assets, files), rows


def write_pack(folder, assets, roster):
    """Write a pack: `assets` with a sound bank that has every character's
    recordings, the voice files and characters.txt. roster: (name, donor,
    object ids, lines) per character, lines being {the game's sound id: a
    recording, or None for a line that is to be silent}. Returns the size of
    assets.bin and the voice files' sizes by name."""
    if len(roster) >= Sounds.SLOTS:
        raise ValueError('%d characters: there is room for %d' % (len(roster), Sounds.SLOTS - 1))
    sounds = Sounds(assets)
    if sounds.sound_count() != SOUNDS_OF_THE_GAME:
        raise ValueError('the sound bank to add to is not the game\'s own')
    maps = [dict((sound_id, sounds.add(recording, number) if recording is not None else 0)
                 for sound_id, recording in sorted(lines.items()))
            for number, (name, donor, ids, lines) in enumerate(roster, 1)]
    taken = set()
    files = sounds.finish(assets, [entry[0] for entry in roster])
    names = [file_name(entry[0], taken) if number in files else '-' for number, entry in enumerate(roster, 1)]
    voices = os.path.join(folder, 'voices')
    os.makedirs(voices, exist_ok=True)
    # Nothing of an earlier pack stays behind: not the single file of the old
    # kind, not the file of a character that is no longer in the list.
    if os.path.exists(os.path.join(folder, 'voices.bin')):
        os.remove(os.path.join(folder, 'voices.bin'))
    for old in os.listdir(voices):
        if old.lower().endswith('.bin') and old not in names:
            os.remove(os.path.join(voices, old))
    sizes = {}
    for number, data in files.items():
        with open(os.path.join(voices, names[number - 1]), 'wb') as f:
            f.write(data)
        sizes[roster[number - 1][0]] = len(data)
    size = assets.write(folder)
    # characters.txt: what 3ds/characters.c reads. The first line's number
    # identifies this exact set; consoles playing together must have the same.
    # "66:640,68:641" maps the game's sound ids to the character's own (0:
    # the line is silent); the last field is its voice file in voices/.
    lines = ['%s|%d|%d|%d|%d|%d|%d|%s|%s' % ((name, donor) + tuple(ids) +
                                            (','.join('%d:%d' % pair for pair in sorted(maps[i].items())) or '-', names[i]))
             for i, (name, donor, ids, voice) in enumerate(roster)]
    check = zlib.crc32('\n'.join(lines).encode()) & 0xffffffff
    with open(os.path.join(folder, 'characters.txt'), 'w', newline='\n') as f:
        f.write('# name|donor|car|hover|plane|select|portrait|sounds|voice file   (tools/import-characters.py)\n')
        f.write('roster %08x\n' % check)
        f.write('\n'.join(lines) + '\n')
    return size, sizes


def check_pack(folder, base, expected=None):
    """Read a pack back and say what is wrong with its sounds (a list of
    sentences; empty when all is well) and how much was looked at.

    Checked: the game's own sounds are the ROM's, byte for byte; every
    recording of an added character is for one of its own lines, is in its
    own voice file where that file's list says, and is no other character's;
    no added sound belongs to nobody; and, for the characters in `expected`
    ({name: lines} as write_pack takes them), the recordings are those.
    """
    problems = []
    assets, sounds, rows = read_pack(folder)
    base_sounds = Sounds(base)
    for sound_id in range(base_sounds.sound_count()):
        place = sounds.place(sound_id)
        if (place is not None and place[0] is not None) or sounds.recording(sound_id) != base_sounds.recording(sound_id):
            problems.append("the game's own sound %d is not the ROM's" % sound_id)
    owners, compared = {}, 0
    for number, (name, donor, ids, mapping, voice_file) in enumerate(rows, 1):
        own, listed = voice_lines(donor), {}
        data = sounds.voice_files.get(number, b'')
        if data:
            magic, version, count, head_name = struct.unpack_from(Sounds.HEADER, data, 0)
            if magic != b'DKRV' or version != Sounds.VERSION or head_name.rstrip(b'\0').decode('utf-8') != name[:15]:
                problems.append("%s: voices/%s is not this character's voice file" % (name, voice_file))
            else:
                at = struct.calcsize(Sounds.HEADER)
                for i in range(count):
                    sound_id, offset, length = struct.unpack_from(Sounds.ENTRY, data, at + 12 * i)
                    listed[sound_id] = (offset, length)
        for game_id, to in sorted(mapping.items()):
            what = '%s, %s (sound %d)' % (name, own.get(game_id, 'not a line'), game_id)
            if game_id not in own:
                problems.append('%s: not one of the lines of %s, its donor' % (what, CHARACTERS[donor]))
            if to == 0:
                continue
            place = sounds.place(to) if base_sounds.sound_count() <= to < sounds.sound_count() else None
            if place is None or place[0] != number:
                problems.append('%s: sound %d is not a recording in its own voice file' % (what, to))
                continue
            if listed.get(to) != place[1:]:
                problems.append("%s: the sound bank and voices/%s disagree about where sound %d is"
                                % (what, voice_file, to))
                continue
            if owners.setdefault(to, number) != number:
                problems.append('%s: sound %d is also %s\'s' % (what, to, rows[owners[to] - 1][0]))
            if expected is not None and name in expected:
                compared += 1
                if sounds.recording(to) != expected[name].get(game_id):
                    problems.append('%s: the recording is not the one meant for it' % what)
        if expected is not None and name in expected:
            for game_id, recording in sorted(expected[name].items()):
                if game_id not in mapping or (recording is None) != (mapping[game_id] == 0):
                    problems.append('%s, %s (sound %d): missing from the pack' % (name, own.get(game_id, '?'), game_id))
        for sound_id in listed:
            if sound_id not in mapping.values():
                problems.append('%s: voices/%s has a recording (sound %d) that no line plays' % (name, voice_file, sound_id))
    for sound_id in range(base_sounds.sound_count(), sounds.sound_count()):
        if sound_id not in owners:
            problems.append('added sound %d belongs to no character' % sound_id)
    return problems, {'characters': len(rows), 'recordings': len(owners), 'compared': compared,
                      'own': base_sounds.sound_count()}


class Importer:
    """Copies characters out of patched ROMs into `out` (the game's assets)."""

    def __init__(self, base_rom, base_folder=None):
        self.rom = base_rom
        self.base = Assets(base_rom, BASE_LUT)
        self.base_sounds = Sounds(self.base)
        self.roster = []        # (name, donor, [car, hover, plane, select, portrait], lines); see write_pack
        if base_folder:
            # An existing pack: its models stay as they are. Its characters'
            # recordings are taken out of it, and its sound bank is put back
            # to the game's own, for write_pack to add everyone's again.
            self.out, sounds, rows = read_pack(base_folder)
            for number, (name, donor, ids, mapping, voice_file) in enumerate(rows, 1):
                lines = {}
                for game_id, to in mapping.items():
                    if to != 0 and (sounds.place(to) or (None,))[0] != number:
                        raise ValueError("%s's sound %d is not in its own voice file" % (name, to))
                    lines[game_id] = sounds.recording(to) if to != 0 else None
                self.roster.append((name, donor, ids, lines))
            self.out.sections[AUDIO] = bytearray(self.base.sections[AUDIO])
            self.out.sections[AUDIO_TABLE] = bytearray(self.base.sections[AUDIO_TABLE])
        else:
            self.out = Assets(base_rom, BASE_LUT)
        self.base_models = self.base.count(MODEL_TABLE)
        # Textures already in the base pack, by content, so they are not added twice.
        self.textures = {}
        for i in range(self.base.count(TEX3D_TABLE)):
            self.textures.setdefault(self.base.record(TEX3D_TABLE, TEX3D, i), i)
        self.free_objects = [i for i in range(511) if u16(self.out.sections[OBJECT_IDS], 2 * i) == UNUSED_OBJECT]

    def character_lines(self, theirs, donor):
        """The recordings a patch has for its character's own lines:
        {the game's sound id: the recording, or None if the patch made the
        line silent}. `theirs` is the patched ROM's Sounds. A line the patch
        left as the donor's is not in it: the donor's voice is not the
        character's. Also returns how many other sounds the patch changed
        (other characters' lines, effects), which are left alone."""
        own, lines, others = voice_lines(donor), {}, 0
        for sound_id in range(min(self.base_sounds.sound_count(), theirs.sound_count())):
            recording = theirs.recording(sound_id)
            if recording != self.base_sounds.recording(sound_id):
                if sound_id in own:
                    lines[sound_id] = recording
                else:
                    others += 1
        return lines, others

    def donors(self, patched):
        """The characters a patch replaces: those whose car models (nearly) all changed."""
        found = []
        for character in range(10):
            header = self.header_of(patched, RACER_OBJECTS[character][0])
            models = [m for m in self.model_ids(header) if m != 0xffffffff]
            changed = sum(1 for m in models if m >= self.base_models or
                          unpack(patched.record(MODEL_TABLE, MODELS, m)) != unpack(self.base.record(MODEL_TABLE, MODELS, m)))
            if changed >= 4:
                found.append(character)
        return found

    @staticmethod
    def header_of(assets, object_id):
        return assets.record(HEADER_TABLE, HEADERS, u16(assets.sections[OBJECT_IDS], 2 * object_id))

    @staticmethod
    def model_ids(header):
        at = u32(header, 0x10)
        return [u32(header, at + 4 * i) for i in range(header[0x55])]

    def add_texture(self, patched, index):
        record = patched.record(TEX3D_TABLE, TEX3D, index)
        if record not in self.textures:
            self.textures[record] = self.out.add(TEX3D_TABLE, TEX3D, record)
        return self.textures[record]

    def add_model(self, patched, index, done):
        if index in done:
            return done[index]
        model = bytearray(unpack(patched.record(MODEL_TABLE, MODELS, index)))
        table = u32(model, 0)               # its textures: 8 bytes each, the id first
        for i in range(u16(model, 0x22)):
            struct.pack_into('>I', model, table + 8 * i, self.add_texture(patched, u32(model, table + 8 * i)))
        new = self.out.add(MODEL_TABLE, MODELS, pack(model))
        # Its animations, added in one run, and the table that says which they are.
        first, end = u16(patched.sections[MODEL_ANIMS], 2 * index), u16(patched.sections[MODEL_ANIMS], 2 * index + 2)
        for anim in range(first, end):
            self.out.add(ANIM_TABLE, ANIMS, patched.record(ANIM_TABLE, ANIMS, anim))
        ends = [u16(self.out.sections[MODEL_ANIMS], 2 * i) for i in range(new + 1)]
        ends.append(self.out.count(ANIM_TABLE))
        self.out.sections[MODEL_ANIMS] = bytearray(pad16(struct.pack('>%dH' % len(ends), *ends)))
        done[index] = new
        return new

    def add_object(self, patched, header, done):
        """A copy of an object header using the patch's models; returns a new object id."""
        header = bytearray(header)
        at = u32(header, 0x10)
        for i, model in enumerate(self.model_ids(header)):
            if model != 0xffffffff:
                struct.pack_into('>I', header, at + 4 * i, self.add_model(patched, model, done))
        index = self.out.add(HEADER_TABLE, HEADERS, header)
        if not self.free_objects:
            raise ValueError('the game has no object ids left (512 at most)')
        object_id = self.free_objects.pop(0)
        struct.pack_into('>H', self.out.sections[OBJECT_IDS], 2 * object_id, index)
        return object_id

    def add_character(self, patched, donor):
        done = {}
        objects = [self.add_object(patched, self.header_of(patched, o), done) for o in RACER_OBJECTS[donor]]
        select = self.add_object(patched, patched.record(HEADER_TABLE, HEADERS, SELECT_HEADERS[donor]), done)
        portrait = patched.record(TEX2D_TABLE, TEX2D, PORTRAITS[donor])
        return objects + [select, self.out.add(TEX2D_TABLE, TEX2D, portrait)], portrait


def display_name(filename):
    name = os.path.splitext(filename)[0]
    name = re.sub(r'\(.*?\)', '', name)                 # (recomp), (native stats)
    name = re.sub(r'\s*-\s*syeo\s*$', '', name.strip(), flags=re.I)
    name = re.sub(r'^DKR-', '', name)
    name = re.sub(r'[-_ ]*(64)?[-_ ]*v?\d+(\.\d+)*$', '', name)  # version tails
    name = re.sub(r'[-_]+', ' ', name).strip()
    known = {'kkrool': 'K. Rool', 'shovelknight': 'Shovel Knight', 'OOT': 'Link', 'dk jr': 'DK Jr.'}
    return known.get(name, name.title())


def read_names(folder):
    # Patches whose file name does not say who is in them.
    names = {'dkr-yoshi-racing-story64v04.xdelta#timber': 'Yoshi',
             'dkr-yoshi-racing-story64v04.xdelta#t.t.': 'Red Yoshi'}
    path = os.path.join(folder, 'names.txt')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if '=' in line and not line.lstrip().startswith('#'):
                key, value = line.split('=', 1)
                names[key.strip().lower()] = value.strip()
    return names


def portrait_image(record):
    """A portrait (40x40, 16-bit colour) as a PIL image, to look at.

    The record is a 32-byte texture header, then the whole texture (header
    again, then the pixels) compressed.
    """
    from PIL import Image
    width, height = record[0], record[1]
    pixels = unpack(record[0x20:])[0x20:]
    image = Image.new('RGBA', (width, height))
    for i in range(width * height):
        v = u16(pixels, 2 * i)
        image.putpixel((i % width, i // width), ((v >> 11 & 31) * 255 // 31, (v >> 6 & 31) * 255 // 31,
                                                 (v >> 1 & 31) * 255 // 31, 255 if v & 1 else 0))
    return image


def patch_characters(importer, folder, names, report=print):
    """The characters in a folder of patches: (file name, character name,
    donor, the patched ROM's assets) for each. names: read_names(folder)."""
    files = sorted((f for f in os.listdir(folder) if f.lower().endswith(('.xdelta', '.vcdiff'))), key=str.lower)
    for filename in files:
        if names.get(filename.lower(), '').lower() == 'skip':
            report('%-45s left out (names.txt)' % filename)
            continue
        try:
            with open(os.path.join(folder, filename), 'rb') as f:
                patched_rom = vcdiff.apply(importer.rom, f.read())
            patched = Assets(patched_rom, find_lut(patched_rom))
            donors = importer.donors(patched)
        except Exception as error:      # one bad patch does not stop the rest
            report('%-45s FAILED: %s' % (filename, error))
            continue
        if not donors:
            report('%-45s no character found in it' % filename)
        for number, donor in enumerate(donors):
            key = '%s#%s' % (filename.lower(), CHARACTERS[donor].lower())
            name = names.get(key) or (names.get(filename.lower()) if len(donors) == 1 else None)
            if name is None:
                name = display_name(filename) + ('' if number == 0 else ' %d' % (number + 1))
            if name.lower() != 'skip':
                yield filename, name.replace('|', ' ')[:15], donor, patched


def describe_lines(donor, lines):
    """In a few words: which of its lines a character has recordings for."""
    own = voice_lines(donor)
    missing = [what for sound_id, what in sorted(own.items()) if sound_id not in lines and what != 'horn']
    silent = [own[sound_id] for sound_id, recording in sorted(lines.items()) if recording is None]
    text = '%d of its %d lines' % (len(lines) - len(silent), len(own))
    if SOUND_HORN + donor not in lines:
        text += ", %s's horn" % CHARACTERS[donor]
    if missing:
        text += ', none for ' + ' '.join(missing)
    if silent:
        text += ', silent: ' + ' '.join(silent)
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rom', default=DEFAULT_ROM, help='Diddy Kong Racing, US 1.0, .z64')
    p.add_argument('--patches', default=DEFAULT_PATCHES, help='folder of .xdelta files')
    p.add_argument('--base-characters', help='existing generated character pack to extend')
    p.add_argument('--out', default=os.path.join(ROOT, 'out', 'characters'))
    args = p.parse_args()

    rom = read_rom(args.rom)
    names = read_names(args.patches)
    try:
        importer = Importer(rom, args.base_characters)
    except (OSError, ValueError) as error:
        sys.exit('could not read the existing character pack: %s' % error)
    kept = set(entry[0] for entry in importer.roster)
    for name in sorted(kept):
        print('%-45s %-16s (kept)' % (args.base_characters, name))
    portraits = []
    for filename, name, donor, patched in patch_characters(importer, args.patches, names):
        if name in kept:
            print('%-45s %-16s is in the pack already' % (filename, name))
            continue
        try:
            ids, portrait = importer.add_character(patched, donor)
            lines, others = importer.character_lines(Sounds(patched), donor)
        except Exception as error:
            print('%-45s FAILED: %s' % (filename, error))
            continue
        importer.roster.append((name, donor, ids, lines))
        portraits.append(portrait)
        print('%-45s %-16s (in place of %s; voice: %s)' % (filename, name, CHARACTERS[donor],
                                                          describe_lines(donor, lines)))

    os.makedirs(args.out, exist_ok=True)
    size, sizes = write_pack(args.out, importer.out, importer.roster)
    # Read back what was written and compare it with what was meant.
    problems, looked = check_pack(args.out, importer.base, dict((entry[0], entry[3]) for entry in importer.roster))
    try:
        from PIL import Image
        sheet = Image.new('RGBA', (40 * max(1, len(portraits)), 40), (40, 40, 40, 255))
        for i, portrait in enumerate(portraits):
            image = portrait_image(portrait)
            sheet.paste(image, (40 * i, 0), image)
        sheet.save(os.path.join(args.out, 'portraits.png'))
    except ImportError:
        pass
    print('\n%d characters, assets.bin %.1f MB, their voice files %.1f MB, %d bananas to unlock them all' %
          (len(importer.roster), size / 1048576.0, sum(sizes.values()) / 1048576.0,
           len(importer.roster) * BANANAS_PER_CHARACTER))
    if problems:
        print('\nTHE VOICES ARE NOT RIGHT (%d things); do not use this pack:' % len(problems))
        for problem in problems[:40]:
            print('  ' + problem)
        sys.exit(1)
    print('voices checked: %d recordings of %d characters, each in its own file and as meant; '
          "the game's own %d sounds as in the ROM" % (looked['recordings'], looked['characters'], looked['own']))
    print('copy assets.bin, assets.lut.bin, characters.txt and the voices folder from %s to sdmc:/3ds/DKR/' % args.out)


if __name__ == '__main__':
    main()
