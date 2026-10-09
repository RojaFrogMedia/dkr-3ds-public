"""The added characters: reading characters/added-characters.pack.

The pack holds what tools/import-characters.py made from character patches
(models, textures, animations, portraits, voices), but none of the game's own
data. It is a difference: for each file the 3DS game needs, a list of steps,
each either "copy so many bytes from this place in the ROM" or "take the
next so many bytes of the pack". So it is of use only with the ROM, and the
files it gives are checked against the check sums it carries.

The files, all for sdmc:/3ds/DKR/:

    assets.bin, assets.lut.bin   the game's assets with the characters added
    characters.txt               the list the game reads (3ds/characters.c)
    voices/<character>.bin       each character's own voice recordings

Layout of the pack (numbers are 32-bit, low byte first):

    'DKRC', version (1)
    SHA-1 of the ROM it is a difference from      20 bytes
    SHA-1 of the body, unpacked                   20 bytes
    the body, packed with zlib:
        number of files
        per file:  length of its name (16-bit), the name (UTF-8, '/' between
                   folders), its size, its SHA-1, its number of steps,
                   the steps: where in the ROM (LITERAL: in the pack), how many bytes
        the pack's own bytes, in the order the steps take them

tools/make-character-pack.py writes one. Only the standard library is used.
"""
import hashlib
import struct
import zlib

MAGIC, VERSION = b'DKRC', 1
HEAD = '<4sI20s20s'
LITERAL = 0xffffffff


class PackError(Exception):
    """The pack cannot be used; the message says why."""


def write(rom, files):
    """The bytes of a pack. files: [(name, [(where in the ROM or LITERAL, bytes or length)])]
    as tools/make-character-pack.py works them out; a literal step carries its bytes."""
    tables, own = [struct.pack('<I', len(files))], []
    for name, steps in files:
        made = hashlib.sha1()
        size = 0
        rows = []
        for where, what in steps:
            data = what if where == LITERAL else rom[where:where + what]
            made.update(data)
            size += len(data)
            rows.append(struct.pack('<II', where, len(data)))
            if where == LITERAL:
                own.append(what)
        encoded = name.encode('utf-8')
        tables.append(struct.pack('<H', len(encoded)) + encoded + struct.pack('<I', size) + made.digest() +
                      struct.pack('<I', len(rows)) + b''.join(rows))
    body = b''.join(tables) + b''.join(own)
    return struct.pack(HEAD, MAGIC, VERSION, hashlib.sha1(rom).digest(), hashlib.sha1(body).digest()) + \
        zlib.compress(body, 9)


def read(pack, rom):
    """The files of a pack, {name: bytes}, made with the ROM (in .z64 order)."""
    if len(pack) < struct.calcsize(HEAD):
        raise PackError('the file is cut short')
    magic, version, of_rom, of_body = struct.unpack_from(HEAD, pack, 0)
    if magic != MAGIC:
        raise PackError('it is not a character pack')
    if version != VERSION:
        raise PackError('it is a pack of version %d; this builder reads version %d' % (version, VERSION))
    if of_rom != hashlib.sha1(rom).digest():
        raise PackError('it was made for another ROM')
    try:
        body = zlib.decompress(pack[struct.calcsize(HEAD):])
    except zlib.error:
        body = b''
    if hashlib.sha1(body).digest() != of_body:
        raise PackError('the file is damaged (cut short, or changed on the way here)')

    at = 4
    listed = []
    for _ in range(struct.unpack_from('<I', body, 0)[0]):
        length = struct.unpack_from('<H', body, at)[0]
        name = body[at + 2:at + 2 + length].decode('utf-8')
        at += 2 + length
        size, digest, count = struct.unpack_from('<I20sI', body, at)
        at += 28
        steps = struct.unpack_from('<%dI' % (2 * count), body, at)
        at += 8 * count
        listed.append((name, size, digest, steps))
    files = {}
    for name, size, digest, steps in listed:
        parts = []
        for where, length in zip(steps[0::2], steps[1::2]):
            if where == LITERAL:
                parts.append(body[at:at + length])
                at += length
            else:
                parts.append(rom[where:where + length])
        data = b''.join(parts)
        if len(data) != size or hashlib.sha1(data).digest() != digest:
            raise PackError('"%s" did not come out as it should' % name)
        if name.startswith('/') or '..' in name.split('/') or '\\' in name or ':' in name:
            raise PackError('"%s" is not a file name a pack may have' % name)
        files[name] = data
    return files


def names(files):
    """The characters' names, in the list's order, from a pack's characters.txt."""
    found = []
    for line in files.get('characters.txt', b'').decode('utf-8').splitlines():
        fields = line.split('|')
        if not line.startswith('#') and len(fields) >= 2:
            found.append(fields[0])
    return found
