"""The pictures the builder makes, all from the ROM's own textures:

    icon        48 x 48    "DKR" in the game's big font, for the menus
    banner      256 x 128  the title logo, for the HOME Menu's banner
    logo        the title logo for the touch screen
    banana      the banana of the race display, for the touch screen
    sky         320 x 240  the touch screen's background (drawn here, not
                           from the ROM: blue sky and cloud)

Only the standard library is used, so the pictures are plain lists of
bytes worked on a pixel at a time; they are small.
"""
import struct
import zlib

import rom as romfile

TITLE_FIRST_TILE = 0x50         # TEXTURE_TITLE_SEGMENT_01 in source/src/menu.h
TITLE_TILES = 11                # ten strips of the logo, then the small "TM"
TITLE_TILE_X = (-75, -60, -45, -30, -15, 0, 15, 30, 45, 60, 75)     # sGameTitleTileOffsets
HUD_SPRITE_BANANA_STATIC = 21   # source/src/game_ui.h


class Picture:
    """Width, height and RGBA pixels (4 bytes each, top row first)."""

    def __init__(self, width, height, pixels=None, colour=(0, 0, 0, 0)):
        self.width, self.height = width, height
        self.pixels = bytearray(pixels) if pixels is not None else bytearray(bytes(colour) * (width * height))

    @classmethod
    def of(cls, texture):
        return cls(texture.width, texture.height, texture.pixels)

    def get(self, x, y):
        at = 4 * (y * self.width + x)
        return self.pixels[at:at + 4]

    def crop(self, x, y, width, height):
        out = Picture(width, height)
        for row in range(height):
            start = 4 * ((y + row) * self.width + x)
            out.pixels[4 * row * width:4 * (row + 1) * width] = self.pixels[start:start + 4 * width]
        return out

    def trimmed(self):
        """Cut down to the part that is not see-through."""
        xs = [x for y in range(self.height) for x in range(self.width) if self.pixels[4 * (y * self.width + x) + 3] > 8]
        ys = [y for y in range(self.height) for x in range(self.width) if self.pixels[4 * (y * self.width + x) + 3] > 8]
        if not xs:
            return self
        return self.crop(min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)

    def draw(self, other, left, top):
        """Lay another picture over this one, through its alpha."""
        for y in range(other.height):
            ty = top + y
            if not 0 <= ty < self.height:
                continue
            for x in range(other.width):
                tx = left + x
                if not 0 <= tx < self.width:
                    continue
                s = 4 * (y * other.width + x)
                alpha = other.pixels[s + 3]
                if alpha == 0:
                    continue
                d = 4 * (ty * self.width + tx)
                if alpha == 255:
                    self.pixels[d:d + 4] = other.pixels[s:s + 4]
                    continue
                under = self.pixels[d + 3] * (255 - alpha) // 255
                total = alpha + under
                for c in range(3):
                    self.pixels[d + c] = (other.pixels[s + c] * alpha + self.pixels[d + c] * under) // total
                self.pixels[d + 3] = total

    def resized(self, width, height):
        """Any size, taking the nearest pixel (the pixels stay sharp squares)."""
        out = Picture(width, height)
        for y in range(height):
            sy = y * self.height // height
            for x in range(width):
                s = 4 * (sy * self.width + x * self.width // width)
                out.pixels[4 * (y * width + x):4 * (y * width + x) + 4] = self.pixels[s:s + 4]
        return out

    def shrunk(self, factor):
        """A whole number of times smaller, each pixel the average of a square."""
        width, height = self.width // factor, self.height // factor
        out = Picture(width, height)
        for y in range(height):
            for x in range(width):
                red = green = blue = alpha = 0
                for sy in range(y * factor, (y + 1) * factor):
                    s = 4 * (sy * self.width + x * factor)
                    for _ in range(factor):
                        a = self.pixels[s + 3]
                        red += self.pixels[s] * a
                        green += self.pixels[s + 1] * a
                        blue += self.pixels[s + 2] * a
                        alpha += a
                        s += 4
                d = 4 * (y * width + x)
                if alpha:
                    out.pixels[d:d + 4] = bytes((red // alpha, green // alpha, blue // alpha,
                                                 alpha // (factor * factor)))
        return out

    def scaled(self, up, down):
        """up/down times the size: sharp enlargement, then averaged down."""
        return self.resized(self.width * up, self.height * up).shrunk(down)

    def png(self):
        """The picture as the bytes of a .png file."""
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        rows = b''.join(b'\0' + bytes(self.pixels[4 * y * self.width:4 * (y + 1) * self.width])
                        for y in range(self.height))
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

    def touch_screen_file(self):
        """The format 3ds/bottomscreen.c reads: "DKRI", width, height, RGBA pixels."""
        return b'DKRI' + struct.pack('<HH', self.width, self.height) + bytes(self.pixels)


def title_logo(assets):
    """The "Diddy Kong Racing" logo as the title screen puts it together."""
    tiles = [Picture.of(romfile.menu_texture(assets, TITLE_FIRST_TILE + i)) for i in range(TITLE_TILES)]
    left = min(TITLE_TILE_X)
    logo = Picture(max(x - left + t.width for x, t in zip(TITLE_TILE_X, tiles)), max(t.height for t in tiles))
    for x, tile in zip(TITLE_TILE_X, tiles):
        logo.draw(tile, x - left, 0)
    return logo.trimmed()


def word(font, text, overlap=2):
    """Letters of a font side by side on one base line, a little over each other."""
    letters = []
    for character in text:
        texture, x, y, width, height = font.letter(character)
        letters.append(Picture.of(texture).crop(x, y, width, height))
    height = max(l.height for l in letters)
    picture = Picture(sum(l.width for l in letters) - overlap * (len(letters) - 1), height)
    x = 0
    for letter in letters:
        picture.draw(letter, x, height - letter.height)
        x += letter.width - overlap
    return picture


def soft_shadow(picture, colour=(8, 16, 60), strength=0.7, radius=4):
    """The picture's shape in one dark colour with soft edges, to go under it."""
    width, height = picture.width + 4 * radius, picture.height + 4 * radius
    alpha = [0] * (width * height)
    for y in range(picture.height):
        for x in range(picture.width):
            alpha[(y + 2 * radius) * width + x + 2 * radius] = picture.pixels[4 * (y * picture.width + x) + 3]
    for _ in range(3):                           # three averages come close to a smooth blur
        for across in (True, False):
            out = [0] * (width * height)
            for y in range(height):
                for x in range(width):
                    total = 0
                    for k in range(-radius // 2, radius // 2 + 1):
                        nx, ny = (x + k, y) if across else (x, y + k)
                        if 0 <= nx < width and 0 <= ny < height:
                            total += alpha[ny * width + nx]
                    out[y * width + x] = total // (radius // 2 * 2 + 1)
            alpha = out
    shade = Picture(width, height)
    for i, a in enumerate(alpha):
        shade.pixels[4 * i:4 * i + 4] = bytes(colour + (int(a * strength),))
    return shade, 2 * radius


def icon(assets):
    """48 x 48: DKR in the game's big font on a blue sky, a chequered flag below."""
    scale = 4                                    # drawn large and averaged down for smooth edges
    size = 48 * scale
    picture = Picture(size, size)
    top, bottom = (22, 70, 190), (120, 200, 250)
    for y in range(size):
        t = y / (size - 1)
        row = bytes(int(a + (b - a) * t) for a, b in zip(top, bottom)) + b'\xff'
        picture.pixels[4 * y * size:4 * (y + 1) * size] = row * size
    square = 4 * scale
    for row in range(2):
        for column in range(12):
            dark = (row + column) % 2 == 0
            tile = Picture(square, square, colour=(20, 20, 28, 255) if dark else (245, 245, 245, 255))
            picture.draw(tile, column * square, size - (2 - row) * square)
    picture.draw(Picture(size, scale, colour=(255, 200, 0, 255)), 0, size - 2 * square - scale)

    text = word(romfile.Font(assets, romfile.BIG_FONT), 'DKR')
    wanted = 44 * scale
    text = text.resized(wanted, round(text.height * wanted / text.width))
    x = (size - text.width) // 2
    y = (size - 2 * square - text.height) // 2
    shade, margin = soft_shadow(text, radius=scale)
    picture.draw(shade, x + scale - margin, y + 2 * scale - margin)
    picture.draw(text, x, y)
    return picture.shrunk(scale)


def centred(picture, width, height):
    out = Picture(width, height)
    out.draw(picture, (width - picture.width) // 2, (height - picture.height) // 2)
    return out


def banner(assets):
    """256 x 128, see-through around the title logo at one and a half times its size."""
    return centred(title_logo(assets).scaled(3, 2), 256, 128)


def touch_logo(assets):
    return title_logo(assets).scaled(3, 2)


def banana(assets):
    """The banana beside the banana count, from the race display's sprite."""
    tiles = romfile.hud_sprite(assets, HUD_SPRITE_BANANA_STATIC)
    left, top = min(t.x for t in tiles), min(t.y for t in tiles)
    picture = Picture(max(t.x - left + t.width for t in tiles), max(t.y - top + t.height for t in tiles))
    for tile in tiles:
        picture.draw(Picture.of(tile), tile.x - left, tile.y - top)
    return picture.trimmed()


def _noise(x, y, seed):
    """A fixed pseudo-random number from 0 to 1 for a whole-numbered point."""
    n = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xffffffff
    n = ((n ^ (n >> 13)) * 1274126177) & 0xffffffff
    return ((n ^ (n >> 16)) & 0xffff) / 65535.0


def _cloud(x, y):
    """Smooth cloudiness from 0 to 1: a few layers of blurred noise."""
    total, weight, size = 0.0, 0.0, 96.0
    for layer in range(4):
        fx, fy = x / size, y / (size * 0.6)
        ix, iy = int(fx), int(fy)
        tx, ty = fx - ix, fy - iy
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        a, b = _noise(ix, iy, layer), _noise(ix + 1, iy, layer)
        c, d = _noise(ix, iy + 1, layer), _noise(ix + 1, iy + 1, layer)
        amount = 1.0 / (1 << layer)
        total += amount * ((a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty)
        weight += amount
        size /= 2
    return total / weight


def sky(width=320, height=240, darken=0.8):
    """The touch screen's background: blue sky with cloud, thicker lower down,
    a little dark so that text on it reads. Not from the ROM."""
    picture = Picture(width, height)
    high, low, white = (36, 86, 188), (70, 130, 214), (226, 236, 244)
    for y in range(height):
        t = y / (height - 1)
        blue = [a + (b - a) * t for a, b in zip(high, low)]
        for x in range(width):
            cover = (_cloud(x, y) - 0.62 + 0.30 * t) / 0.30
            cover = 0.0 if cover < 0 else 1.0 if cover > 1 else cover * cover * (3 - 2 * cover)
            at = 4 * (y * width + x)
            picture.pixels[at:at + 4] = bytes(int((b + (w - b) * cover * 0.85) * darken)
                                              for b, w in zip(blue, white)) + b'\xff'
    return picture
