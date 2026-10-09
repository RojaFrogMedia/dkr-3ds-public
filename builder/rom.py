"""Reading the Diddy Kong Racing ROM: checking it, and taking out of it the
asset file the 3DS game reads and the pictures the builder needs.

Only the standard library is used. Nothing here writes anything.

The ROM
    Nintendo 64 ROM files come in three byte orders (.z64, .v64, .n64). All
    three are accepted and brought to .z64 order before anything else. The
    game supported is the first US release:

        Diddy Kong Racing (USA) (En,Fr)   version 1.0
        SHA-1 0cb115d8716dbbc2922fda38e533b9fe63bb9670   (in .z64 order)

The asset file
    Everything in the cartridge that is not program code (models, textures,
    levels, sound, text) lies in one stretch of the ROM: a directory of 208
    bytes (a count of 50, then 51 offsets) followed by the 50 sections it
    lists. The 3DS game reads exactly that, as two files:

        assets.lut.bin   the directory
        assets.bin       the 50 sections
"""
import hashlib
import io
import struct
import zipfile
import zlib

ROM_SHA1 = '0cb115d8716dbbc2922fda38e533b9fe63bb9670'
ROM_NAME = 'Diddy Kong Racing (USA) (En,Fr), version 1.0'
ROM_BYTES = 12 * 1024 * 1024

ASSET_DIRECTORY = 0xecb60       # where the directory is in the ROM
DIRECTORY_BYTES = 208
SECTIONS = 50

# The sections used here (the full list is the enum at the top of
# source/include/asset_enums.h).
TEXTURES_3D, TEXTURES_3D_TABLE = 2, 3
TEXTURES_2D, TEXTURES_2D_TABLE = 4, 5
SPRITES, SPRITES_TABLE = 12, 13
HUD_ELEMENT_IDS = 17
MENU_ELEMENT_IDS = 18
FONTS = 44


class RomError(Exception):
    """The file is not the ROM the game needs; the message says why."""


def u16(data, at):
    return struct.unpack_from('>H', data, at)[0]


def s16(data, at):
    return struct.unpack_from('>h', data, at)[0]


def u32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def to_z64(data):
    """The ROM in .z64 (big-endian) byte order, whichever order it came in."""
    start = data[:4]
    if start == b'\x80\x37\x12\x40':
        return bytes(data)
    if len(data) % 4:
        raise RomError('this is not a Nintendo 64 ROM (its length is not a multiple of 4 bytes)')
    if start == b'\x37\x80\x40\x12':            # .v64: every two bytes swapped
        out = bytearray(len(data))
        out[0::2] = data[1::2]
        out[1::2] = data[0::2]
        return bytes(out)
    if start == b'\x40\x12\x37\x80':            # .n64: every four bytes reversed
        out = bytearray(len(data))
        out[0::4] = data[3::4]
        out[1::4] = data[2::4]
        out[2::4] = data[1::4]
        out[3::4] = data[0::4]
        return bytes(out)
    raise RomError('this is not a Nintendo 64 ROM (it does not begin the way one does)')


def describe(rom):
    """What a ROM in .z64 order says about itself, for an error message."""
    title = rom[0x20:0x34].decode('ascii', 'replace').strip()
    region = {0x45: 'USA', 0x50: 'Europe', 0x4a: 'Japan'}.get(rom[0x3e], 'region code %#x' % rom[0x3e])
    return '"%s", %s, version 1.%d' % (title, region, rom[0x3f])


def read_rom_file(path):
    """The bytes of a ROM file. A .zip is opened and the ROM inside it taken."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] == b'PK':
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            inside = [n for n in archive.namelist() if n.lower().endswith(('.z64', '.n64', '.v64', '.rom', '.bin'))]
            if len(inside) != 1:
                raise RomError('the .zip has %d ROM files in it; it must have exactly one' % len(inside))
            data = archive.read(inside[0])
    return data


def load_rom(path):
    """The checked ROM in .z64 order, or RomError saying what is wrong with it."""
    rom = to_z64(read_rom_file(path))
    digest = hashlib.sha1(rom).hexdigest()
    if digest == ROM_SHA1:
        return rom
    found = describe(rom) if len(rom) >= 0x40 else 'a file of %d bytes' % len(rom)
    if 'Diddy Kong Racing' not in found:
        raise RomError('this is not Diddy Kong Racing: the ROM calls itself %s.' % found)
    if len(rom) != ROM_BYTES:
        raise RomError('the ROM is %d bytes long and should be %d: it is cut short, padded or modified.'
                       % (len(rom), ROM_BYTES))
    if rom[0x3e] != 0x45 or rom[0x3f] != 0:
        raise RomError('this is Diddy Kong Racing, but the wrong release: %s.\n'
                       'The game needs %s.' % (found, ROM_NAME))
    raise RomError('this is %s, but its contents are not the original\'s\n'
                   '(SHA-1 %s, expected %s).\n'
                   'It is a patched, hacked or damaged copy; an unmodified one is needed.' % (found, digest, ROM_SHA1))


class Assets:
    """The asset directory and its 50 sections, as they are in the ROM."""

    def __init__(self, rom):
        if u32(rom, ASSET_DIRECTORY) != SECTIONS:
            raise RomError('the ROM has no asset directory where it should be')
        self.directory = rom[ASSET_DIRECTORY:ASSET_DIRECTORY + DIRECTORY_BYTES]
        offsets = [u32(self.directory, 4 + 4 * i) for i in range(SECTIONS + 1)]
        start = ASSET_DIRECTORY + DIRECTORY_BYTES
        self.data = rom[start:start + offsets[-1]]
        self.sections = [self.data[a:b] for a, b in zip(offsets, offsets[1:])]
        self._tables = {}

    def table(self, section):
        """The offsets of a table section, up to its end mark."""
        if section not in self._tables:
            data, out = self.sections[section], []
            for at in range(0, len(data), 4):
                value = u32(data, at)
                if value == 0xffffffff:
                    break
                out.append(value)
            self._tables[section] = out
        return self._tables[section]

    def record(self, table, section, index):
        offsets = self.table(table)
        return self.sections[section][offsets[index]:offsets[index + 1]]


# --- Textures -------------------------------------------------------------
# A texture record is a header of 32 bytes and the pixels; if the header says
# so (byte 0x1D), what follows the first header is the whole record again,
# compressed: 4 bytes of length (little-endian), one byte, then raw deflate.
# A record may hold several textures one after another (an animation); each
# has its own header, whose bytes 0x16-0x17 give its length.

def inflate(packed):
    size = int.from_bytes(packed[:4], 'little')
    data = zlib.decompressobj(-15).decompress(packed[5:], size + 1)
    if len(data) != size:
        raise RomError('a compressed record of the ROM does not unpack')
    return data


def _five(value):
    return value * 255 // 31


def decode_pixels(width, height, pixel_format, data):
    """Pixels of one of the N64's formats as RGBA, 4 bytes a pixel, top row first."""
    count = width * height
    out = bytearray(4 * count)
    if pixel_format == 0:                       # RGBA32
        out[:] = data[:4 * count]
    elif pixel_format == 1:                     # RGBA16: 5 bits each, 1 of alpha
        for i in range(count):
            v = u16(data, 2 * i)
            out[4 * i:4 * i + 4] = bytes((_five(v >> 11 & 31), _five(v >> 6 & 31), _five(v >> 1 & 31),
                                          255 if v & 1 else 0))
    elif pixel_format == 2:                     # I8: brightness, also used as alpha
        for i in range(count):
            out[4 * i:4 * i + 4] = bytes((data[i],)) * 4
    elif pixel_format == 3:                     # I4
        for i in range(count):
            v = (data[i // 2] >> (0 if i & 1 else 4) & 15) * 17
            out[4 * i:4 * i + 4] = bytes((v,)) * 4
    elif pixel_format == 4:                     # IA16: brightness, alpha
        for i in range(count):
            v = data[2 * i]
            out[4 * i:4 * i + 4] = bytes((v, v, v, data[2 * i + 1]))
    elif pixel_format == 5:                     # IA8: 4 bits each
        for i in range(count):
            v = (data[i] >> 4) * 17
            out[4 * i:4 * i + 4] = bytes((v, v, v, (data[i] & 15) * 17))
    elif pixel_format == 6:                     # IA4: 3 bits of brightness, 1 of alpha
        for i in range(count):
            n = data[i // 2] >> (0 if i & 1 else 4) & 15
            v = (n >> 1) * 255 // 7
            out[4 * i:4 * i + 4] = bytes((v, v, v, 255 if n & 1 else 0))
    else:
        raise RomError('texture format %d is not one this tool reads' % pixel_format)
    return out


class Texture:
    """One decoded texture: its size, where a sprite puts it, and RGBA pixels."""

    def __init__(self, header, pixels):
        self.width, self.height = header[0], header[1]
        self.x, self.y = struct.unpack_from('bb', header, 3)
        self.pixels = decode_pixels(self.width, self.height, header[2] & 15, pixels)


def textures(assets, texture_id):
    """Every texture of a record. Ids from 0x8000 are the 3D models' textures."""
    if texture_id & 0x8000:
        record = assets.record(TEXTURES_3D_TABLE, TEXTURES_3D, texture_id & 0x7fff)
    else:
        record = assets.record(TEXTURES_2D_TABLE, TEXTURES_2D, texture_id)
    data = inflate(record[0x20:]) if record[0x1d] else record
    out, at = [], 0
    for _ in range(max(1, u16(record, 0x12) >> 8)):
        out.append(Texture(data[at:at + 0x20], data[at + 0x20:]))
        at += u16(data, at + 0x16)
    return out


def texture(assets, texture_id):
    return textures(assets, texture_id)[0]


def menu_texture(assets, element):
    """A texture of the menus by its number in the game (TEXTURE_* in source/src/menu.h)."""
    value = u16(assets.sections[MENU_ELEMENT_IDS], 2 * element)
    if value & 0xc000 != 0xc000:
        raise RomError('menu element %#x is not a texture' % element)
    return texture(assets, value & 0x3fff)


def hud_sprite(assets, element, frame=0):
    """One frame of a sprite of the race display (HUD_SPRITE_* in source/src/game_ui.h):
    a list of textures, each to be drawn at its own x and y."""
    value = u16(assets.sections[HUD_ELEMENT_IDS], 2 * element)
    if value & 0xc000 != 0x8000:
        raise RomError('race display element %d is not a sprite' % element)
    record = assets.record(SPRITES_TABLE, SPRITES, value & 0x3fff)
    base, frames = s16(record, 0), s16(record, 2)
    if not 0 <= frame < frames:
        raise RomError('the sprite has no frame %d' % frame)
    first, end = record[12 + frame], record[12 + frame + 1]
    return [texture(assets, (base + i) & 0xffff) for i in range(first, end)]


# --- Fonts ----------------------------------------------------------------
# The fonts section: a count, then 0x400 bytes per font. At 0x40 the font's
# texture ids (16 bits each, up to 32, -1 ends them); at 0x100 eight bytes
# for each of the 96 characters from the space onwards: which of the font's
# textures, how far to move on after it, two unused, where in the texture
# the letter starts (x, y), and its width and height there.

FONT_BYTES = 0x400
BIG_FONT = 2                    # ASSET_FONTS_BIGFONT: the coloured menu headings


class Font:
    def __init__(self, assets, number):
        data = assets.sections[FONTS]
        if not 0 <= number < u32(data, 0):
            raise RomError('the ROM has no font %d' % number)
        self.assets = assets
        self.data = data[4 + FONT_BYTES * number:4 + FONT_BYTES * (number + 1)]
        ids = [s16(self.data, 0x40 + 2 * i) for i in range(32)]
        self.texture_ids = ids[:ids.index(-1)] if -1 in ids else ids
        self._textures = {}

    def letter(self, character):
        """(texture, x, y, width, height) of a character's picture."""
        at = 0x100 + 8 * (ord(character) - 32)
        index, _advance, _, _, x, y, width, height = self.data[at:at + 8]
        if index == 0xff or index >= len(self.texture_ids):
            raise RomError('the font has no letter %r' % character)
        if index not in self._textures:
            self._textures[index] = texture(self.assets, self.texture_ids[index] & 0xffff)
        return self._textures[index], x, y, width, height
