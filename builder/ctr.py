"""The Nintendo 3DS file formats the builder writes.

The program itself is already built (prebuilt/DKR.3dsx and prebuilt/DKR.cia,
made by tools/make-prebuilt.sh with devkitARM, makerom and bannertool). Both
carry blank pictures. This module puts the real ones in:

    DKR.3dsx   the icon file (SMDH) inside it is replaced, nothing else
    DKR.cia    the banner and the icon inside it are replaced, then every
               check sum that covers them is worked out again and the two
               signatures that cover those are made again

so that the result is what makerom, bannertool and 3dsxtool write when they
are given the same pictures (tools/check-builder.sh runs them and compares).

Signatures. A homebrew .cia cannot carry Nintendo's signatures; makerom signs
with a test key that is printed in its source code, and a console with
custom firmware does not check them. The same test key is used here
(testkey.py), which is why the result can equal makerom's.
"""
import hashlib
import struct

import testkey

MEDIA_UNIT = 0x200


# --- Pixels ---------------------------------------------------------------
# The 3DS stores pictures in tiles of 8 x 8 pixels, and the pixels of a tile
# in a Z-shaped order (x and y bits interleaved).

def tiles(picture, pixel_format):
    """A picture as 16-bit tiled pixels: 'rgb565' (icons) or 'rgba4444' (banner)."""
    out = bytearray(2 * picture.width * picture.height)
    for y in range(picture.height):
        for x in range(picture.width):
            index = ((((y >> 3) * (picture.width >> 3) + (x >> 3)) << 6)
                     + ((x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3))
            red, green, blue, alpha = picture.get(x, y)
            if pixel_format == 'rgb565':
                red, green, blue = red * alpha // 255, green * alpha // 255, blue * alpha // 255
                colour = (red & ~7) << 8 | (green & ~3) << 3 | blue >> 3
            else:
                colour = (red & ~15) << 8 | (green & ~15) << 4 | (blue & ~15) | alpha >> 4
            struct.pack_into('<H', out, 2 * index, colour)
    return bytes(out)


# --- The icon file (SMDH) -------------------------------------------------
# 0x36C0 bytes: names in sixteen languages, settings, then the icon twice,
# 24 x 24 at 0x2040 and 48 x 48 at 0x24C0.

SMDH_BYTES = 0x36c0
SMDH_SMALL, SMDH_LARGE = 0x2040, 0x24c0


def smdh_with_icon(smdh, icon):
    """An icon file with its two pictures replaced by a 48 x 48 picture."""
    if len(smdh) != SMDH_BYTES or smdh[:4] != b'SMDH':
        raise ValueError('not an icon file (SMDH)')
    if (icon.width, icon.height) != (48, 48):
        raise ValueError('the icon must be 48 x 48')
    out = bytearray(smdh)
    out[SMDH_SMALL:SMDH_LARGE] = tiles(icon.shrunk(2), 'rgb565')
    out[SMDH_LARGE:SMDH_BYTES] = tiles(icon, 'rgb565')
    return bytes(out)


# --- LZ11 compression (the banner's model is stored with it) --------------

def lz11_unpack(data):
    if data[0] != 0x11:
        raise ValueError('not LZ11 data')
    size = int.from_bytes(data[1:4], 'little')
    out = bytearray()
    at = 4
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if not flags >> bit & 1:
                out.append(data[at])
                at += 1
                continue
            kind = data[at] >> 4
            if kind == 0:
                length = ((data[at] & 15) << 4 | data[at + 1] >> 4) + 0x11
                back = ((data[at + 1] & 15) << 8 | data[at + 2]) + 1
                at += 3
            elif kind == 1:
                length = ((data[at] & 15) << 12 | data[at + 1] << 4 | data[at + 2] >> 4) + 0x111
                back = ((data[at + 2] & 15) << 8 | data[at + 3]) + 1
                at += 4
            else:
                length = kind + 1
                back = ((data[at] & 15) << 8 | data[at + 1]) + 1
                at += 2
            for _ in range(length):
                out.append(out[-back])
    return bytes(out)


def lz11_pack(data):
    """LZ11 with the two common kinds of back-reference (3 to 272 bytes, up
    to 4096 bytes back). Any LZ11 reader unpacks it."""
    size = len(data)
    if size >= 1 << 24:
        raise ValueError('too large for LZ11')
    out = bytearray(b'\x11' + size.to_bytes(3, 'little'))
    seen = {}                                   # three bytes -> where they were last seen, newest last
    at = 0

    def remember(position):
        if position + 3 <= size:
            seen.setdefault(data[position:position + 3], []).append(position)

    while at < size:
        flag_at = len(out)
        out.append(0)
        for bit in range(7, -1, -1):
            if at >= size:
                break
            best_length, best_back = 0, 0
            places = seen.get(data[at:at + 3], ())
            limit = min(0x110, size - at)
            for place in places[-1:-33:-1]:
                if at - place > 0x1000:
                    break
                length = 3
                while length < limit and data[place + length] == data[at + length]:
                    length += 1
                if length > best_length:
                    best_length, best_back = length, at - place
                    if length == limit:
                        break
            if best_length >= 3:
                out[flag_at] |= 1 << bit
                back = best_back - 1
                if best_length > 0x10:
                    value = best_length - 0x11
                    out += bytes((value >> 4, (value & 15) << 4 | back >> 8, back & 0xff))
                else:
                    out += bytes(((best_length - 1) << 4 | back >> 8, back & 0xff))
                for position in range(at, at + best_length):
                    remember(position)
                at += best_length
            else:
                out.append(data[at])
                remember(at)
                at += 1
    out += bytes(-len(out) % 4)
    return bytes(out)


# --- The banner (CBMD) ----------------------------------------------------
# A header of 0x88 bytes ("CBMD", then where the model is and, at 0x84, where
# the sound is), the model compressed, the sound. The model is bannertool's
# own flat panel: 0x1580 bytes of model, then its picture, 256 x 128.

BANNER_HEADER = 0x88
BANNER_MODEL_BYTES = 0x1580
BANNER_SIZE = (256, 128)


def banner_parts(banner):
    """(the model unpacked, the sound) of a banner."""
    if banner[:4] != b'CBMD' or struct.unpack_from('<I', banner, 8)[0] != BANNER_HEADER:
        raise ValueError('not a banner (CBMD)')
    sound_at = struct.unpack_from('<I', banner, 0x84)[0]
    return lz11_unpack(banner[BANNER_HEADER:sound_at]), banner[sound_at:]


def banner_with_picture(banner, picture):
    """A banner with the picture on its panel replaced."""
    if (picture.width, picture.height) != BANNER_SIZE:
        raise ValueError('the banner picture must be %d x %d' % BANNER_SIZE)
    model, sound = banner_parts(banner)
    pixels = tiles(picture, 'rgba4444')
    if len(model) != BANNER_MODEL_BYTES + len(pixels):
        raise ValueError('the banner is not the flat kind this tool knows')
    packed = lz11_pack(model[:BANNER_MODEL_BYTES] + pixels)
    sound_at = (BANNER_HEADER + len(packed) + 15) & ~15
    header = bytearray(banner[:BANNER_HEADER])
    struct.pack_into('<I', header, 0x84, sound_at)
    return bytes(header) + packed + bytes(sound_at - BANNER_HEADER - len(packed)) + sound


# --- The Homebrew Launcher's program (.3dsx) ------------------------------
# With the longer of its two headers (44 bytes) it says at 0x20 where an icon
# file is and how long.

def threedsx_with_icon(program, smdh):
    header_bytes = struct.unpack_from('<H', program, 4)[0]
    if program[:4] != b'3DSX' or header_bytes < 44:
        raise ValueError('not a .3dsx with room for an icon')
    at, size = struct.unpack_from('<II', program, 0x20)
    if size != len(smdh) or at + size > len(program):
        raise ValueError('the .3dsx has no icon file of the usual size')
    return program[:at] + smdh + program[at + size:]


# --- Signatures -----------------------------------------------------------

SHA256_MARK = bytes.fromhex('3031300d060960864801650304020105000420')


def sign(data):
    """RSA-2048 signature over SHA-256 (PKCS #1 v1.5) with makerom's test key."""
    digest = hashlib.sha256(data).digest()
    padded = b'\x00\x01' + b'\xff' * (256 - 3 - len(SHA256_MARK) - len(digest)) + b'\x00' + SHA256_MARK + digest
    number = pow(int.from_bytes(padded, 'big'), testkey.PRIVATE_EXPONENT, testkey.MODULUS)
    return number.to_bytes(256, 'big')


def signed_by_test_key(signature, data):
    digest = hashlib.sha256(data).digest()
    number = pow(int.from_bytes(signature, 'big'), 65537, testkey.MODULUS)
    return number.to_bytes(256, 'big').endswith(b'\x00' + SHA256_MARK + digest)


# --- The installable title (.cia) -----------------------------------------
# A .cia is: a header, certificates, a ticket, the title's description (TMD),
# the program (an "NCCH"), and a little extra for the menus (meta), each
# padded to 64 bytes. Inside the NCCH the last part is a small file system
# ("ExeFS") holding four files: .code, banner, icon, logo.
#
# Changing the banner and the icon changes, from the inside out:
#   the ExeFS            its header lists each file's place, size and SHA-256
#   the NCCH header      size, the ExeFS' size and its header's SHA-256; signed
#   the TMD              the program's size and SHA-256, a SHA-256 over that
#                        record, a SHA-256 over that; its header is signed
#   the .cia header      the program's size
#   the meta part        has a copy of the icon file at 0x400

def pad(data, unit):
    return bytes(data) + bytes(-len(data) % unit)


def exefs_files(exefs):
    """[(name, contents)] of an ExeFS."""
    files = []
    for i in range(10):
        entry = exefs[16 * i:16 * i + 16]
        if entry[0]:
            at, size = struct.unpack_from('<II', entry, 8)
            files.append((entry[:8], exefs[MEDIA_UNIT + at:MEDIA_UNIT + at + size]))
    return files


def exefs_build(files, reserved):
    """An ExeFS: the header, then each file padded to 0x200 bytes."""
    header = bytearray(MEDIA_UNIT)
    header[0xa0:0xc0] = reserved
    body = b''
    for i, (name, contents) in enumerate(files):
        header[16 * i:16 * i + 16] = name + struct.pack('<II', len(body), len(contents))
        header[MEDIA_UNIT - 32 * (i + 1):MEDIA_UNIT - 32 * i] = hashlib.sha256(contents).digest()
        body += pad(contents, MEDIA_UNIT)
    return bytes(header) + body


def ncch_with_files(ncch, replace):
    """The program with files of its ExeFS replaced ({name: contents})."""
    if ncch[0x100:0x104] != b'NCCH':
        raise ValueError('the .cia does not hold a program (NCCH)')
    if not ncch[0x188 + 7] & 4:
        raise ValueError('the program is encrypted')
    exefs_at, exefs_units, hashed_units = struct.unpack_from('<III', ncch, 0x1a0)
    exefs_at *= MEDIA_UNIT
    if exefs_at + exefs_units * MEDIA_UNIT != len(ncch) or hashed_units != 1:
        raise ValueError('the program is not laid out the way this tool knows')
    exefs = ncch[exefs_at:]
    files = exefs_files(exefs)
    names = [name.rstrip(b'\0').decode() for name, _ in files]
    for name in replace:
        if name not in names:
            raise ValueError('the program has no file "%s"' % name)
    files = [(raw, replace.get(name, contents)) for name, (raw, contents) in zip(names, files)]
    exefs = exefs_build(files, exefs[0xa0:0xc0])
    out = bytearray(ncch[:exefs_at] + exefs)
    struct.pack_into('<I', out, 0x104, len(out) // MEDIA_UNIT)
    struct.pack_into('<I', out, 0x1a4, len(exefs) // MEDIA_UNIT)
    out[0x1c0:0x1e0] = hashlib.sha256(exefs[:MEDIA_UNIT]).digest()
    out[:0x100] = sign(out[0x100:0x200])
    return bytes(out)


TMD_HEADER, TMD_HEADER_BYTES = 0x140, 0xc4      # after the signature
TMD_INFO_RECORDS = TMD_HEADER + TMD_HEADER_BYTES
TMD_CHUNKS = TMD_INFO_RECORDS + 64 * 0x24
META_ICON = 0x400


class Cia:
    def __init__(self, data):
        (header_bytes, self.kind, self.version, certs, ticket, tmd, meta,
         content) = struct.unpack_from('<IHHIIIIQ', data, 0)
        self.header = data[:header_bytes]
        at = len(pad(self.header, 64))
        parts = []
        for size in (certs, ticket, tmd, content, meta):
            parts.append(data[at:at + size])
            at += len(pad(parts[-1], 64))
        self.certs, self.ticket, self.tmd, self.content, self.meta = parts
        if struct.unpack_from('>H', self.tmd, TMD_HEADER + 0x9e)[0] != 1:
            raise ValueError('the .cia does not hold exactly one program')
        if not (signed_by_test_key(self.tmd[4:0x104], self.tmd[TMD_HEADER:TMD_INFO_RECORDS])
                and signed_by_test_key(self.content[:0x100], self.content[0x100:0x200])):
            raise ValueError('the .cia was not signed with makerom\'s test key')

    def with_pictures(self, banner, smdh):
        """The bytes of this .cia with another banner and icon file."""
        content = ncch_with_files(self.content, {'banner': banner, 'icon': smdh})

        tmd = bytearray(self.tmd)
        struct.pack_into('>Q', tmd, TMD_CHUNKS + 8, len(content))
        tmd[TMD_CHUNKS + 0x10:TMD_CHUNKS + 0x30] = hashlib.sha256(content).digest()
        tmd[TMD_INFO_RECORDS + 4:TMD_INFO_RECORDS + 0x24] = hashlib.sha256(tmd[TMD_CHUNKS:TMD_CHUNKS + 0x30]).digest()
        tmd[TMD_HEADER + 0xa4:TMD_HEADER + 0xc4] = hashlib.sha256(tmd[TMD_INFO_RECORDS:TMD_CHUNKS]).digest()
        tmd[4:0x104] = sign(tmd[TMD_HEADER:TMD_INFO_RECORDS])

        meta = bytearray(self.meta)
        meta[META_ICON:META_ICON + SMDH_BYTES] = smdh

        header = bytearray(self.header)
        struct.pack_into('<Q', header, 0x18, len(content))
        # The last part is written as long as it is; the others are padded.
        return b''.join(pad(part, 64) for part in (header, self.certs, self.ticket, tmd, content)) + bytes(meta)

    def faults(self):
        """What is wrong with this .cia's check sums and signatures: a list, empty if nothing."""
        out = []
        tmd, content = self.tmd, self.content

        def same(what, digest, data):
            if hashlib.sha256(data).digest() != digest:
                out.append(what)

        if struct.unpack_from('<Q', self.header, 0x18)[0] != len(content):
            out.append('the size in the .cia header')
        if struct.unpack_from('>Q', tmd, TMD_CHUNKS + 8)[0] != len(content):
            out.append('the size in the TMD')
        same('the program\'s check sum', tmd[TMD_CHUNKS + 0x10:TMD_CHUNKS + 0x30], content)
        same('the TMD\'s content record', tmd[TMD_INFO_RECORDS + 4:TMD_INFO_RECORDS + 0x24],
             tmd[TMD_CHUNKS:TMD_CHUNKS + 0x30])
        same('the TMD\'s list of records', tmd[TMD_HEADER + 0xa4:TMD_HEADER + 0xc4], tmd[TMD_INFO_RECORDS:TMD_CHUNKS])
        if struct.unpack_from('<I', content, 0x104)[0] * MEDIA_UNIT != len(content):
            out.append('the size in the program\'s header')
        exefs_at = struct.unpack_from('<I', content, 0x1a0)[0] * MEDIA_UNIT
        exefs = content[exefs_at:]
        same('the ExeFS header\'s check sum', content[0x1c0:0x1e0], exefs[:MEDIA_UNIT])
        for i, (name, data) in enumerate(exefs_files(exefs)):
            same('the check sum of %s' % name.rstrip(b'\0').decode(),
                 exefs[MEDIA_UNIT - 32 * (i + 1):MEDIA_UNIT - 32 * i], data)
        if not signed_by_test_key(tmd[4:0x104], tmd[TMD_HEADER:TMD_INFO_RECORDS]):
            out.append('the TMD\'s signature')
        if not signed_by_test_key(content[:0x100], content[0x100:0x200]):
            out.append('the program\'s signature')
        if self.meta[META_ICON:META_ICON + SMDH_BYTES] != self.program_files().get('icon'):
            out.append('the icon copy in the meta part')
        return out

    def program_files(self):
        """{name: contents} of the program's ExeFS, to check a result."""
        exefs_at = struct.unpack_from('<I', self.content, 0x1a0)[0] * MEDIA_UNIT
        return {name.rstrip(b'\0').decode(): contents for name, contents in exefs_files(self.content[exefs_at:])}
