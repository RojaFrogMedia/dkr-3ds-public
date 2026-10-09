"""Apply an xdelta3 (.xdelta / VCDIFF, RFC 3284) patch, in plain Python.

    python tools/vcdiff.py <source file> <patch> <output file>

    import vcdiff
    patched = vcdiff.apply(source_bytes, patch_bytes)

Written because the character patches are made by xdelta3 with its LZMA
"secondary compression", which the ready-made Python decoders do not read.
Only what xdelta3 writes is supported: the default instruction table, and
sections either plain or LZMA-compressed.
"""
import lzma
import sys
import zlib

VCD_SOURCE, VCD_TARGET, VCD_ADLER32 = 1, 2, 4
RUN, ADD, COPY = 1, 2, 3


def _default_table():
    """The 256 instruction pairs of RFC 3284 section 5.6: (type, size, mode) twice."""
    none = (0, 0, 0)
    table = [((RUN, 0, 0), none)]
    table += [((ADD, size, 0), none) for size in range(0, 18)]
    for mode in range(9):
        table.append(((COPY, 0, mode), none))
        table += [((COPY, size, mode), none) for size in range(4, 19)]
    for mode in range(6):
        for add in range(1, 5):
            for copy in range(4, 7):
                table.append(((ADD, add, 0), (COPY, copy, mode)))
    for mode in range(6, 9):
        for add in range(1, 5):
            table.append(((ADD, add, 0), (COPY, 4, mode)))
    for mode in range(9):
        table.append(((COPY, 4, mode), (ADD, 1, 0)))
    assert len(table) == 256
    return table


TABLE = _default_table()


class _Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def byte(self):
        self.pos += 1
        return self.data[self.pos - 1]

    def number(self):
        """A base-128 number, most significant group first."""
        value = 0
        while True:
            b = self.byte()
            value = (value << 7) | (b & 0x7F)
            if not b & 0x80:
                return value

    def take(self, count):
        self.pos += count
        return self.data[self.pos - count:self.pos]

    def done(self):
        return self.pos >= len(self.data)


def apply(source, patch):
    r = _Reader(patch)
    if r.take(3) != b'\xd6\xc3\xc4':
        raise ValueError('not a VCDIFF / xdelta file')
    r.byte()                                    # version
    header = r.byte()
    compressor = r.byte() if header & 1 else 0
    if header & 2:
        raise ValueError('patch carries its own instruction table (not supported)')
    if header & 4:
        r.take(r.number())                      # xdelta3's application header
    if compressor not in (0, 2):
        raise ValueError('secondary compressor %d is not supported (only LZMA)' % compressor)
    # xdelta3 keeps one LZMA stream per section kind for the whole file.
    streams = [lzma.LZMADecompressor(lzma.FORMAT_XZ) for _ in range(3)]

    out = bytearray()
    while not r.done():
        indicator = r.byte()
        segment = b''
        if indicator & (VCD_SOURCE | VCD_TARGET):
            length, position = r.number(), r.number()
            base = source if indicator & VCD_SOURCE else out
            segment = bytes(base[position:position + length])
        r.number()                              # length of the delta encoding
        window_length = r.number()
        compressed = r.byte()
        lengths = [r.number(), r.number(), r.number()]
        checksum = int.from_bytes(r.take(4), 'big') if indicator & VCD_ADLER32 else None
        sections = []
        for kind in range(3):
            raw = r.take(lengths[kind])
            if compressed & (1 << kind):
                inner = _Reader(raw)
                size = inner.number()
                raw = streams[kind].decompress(raw[inner.pos:], size)
                if len(raw) != size:
                    raise ValueError('LZMA section came out short')
            sections.append(raw)
        window = _window(segment, window_length, *sections)
        if checksum is not None and zlib.adler32(window) != checksum:
            raise ValueError('checksum mismatch: wrong source file for this patch?')
        out += window
    return bytes(out)


def _window(segment, length, data, inst, addr):
    data, inst, addr = _Reader(data), _Reader(inst), _Reader(addr)
    out = bytearray()
    near, same, slot = [0] * 4, [0] * 768, 0
    seg = len(segment)
    while not inst.done():
        for kind, size, mode in TABLE[inst.byte()]:
            if kind == 0:
                continue
            if size == 0:
                size = inst.number()
            if kind == ADD:
                out += data.take(size)
            elif kind == RUN:
                out += bytes([data.byte()]) * size
            else:
                here = seg + len(out)
                if mode == 0:
                    at = addr.number()
                elif mode == 1:
                    at = here - addr.number()
                elif mode < 6:
                    at = near[mode - 2] + addr.number()
                else:
                    at = same[(mode - 6) * 256 + addr.byte()]
                near[slot] = at
                slot = (slot + 1) & 3
                same[at % 768] = at
                if at + size <= seg:
                    out += segment[at:at + size]
                else:
                    for i in range(at, at + size):      # may run into what it is writing
                        out.append(segment[i] if i < seg else out[i - seg])
    if len(out) != length:
        raise ValueError('window came out %d bytes, expected %d' % (len(out), length))
    return out


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    result = apply(open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read())
    open(sys.argv[3], 'wb').write(result)
    print('%s: %d bytes' % (sys.argv[3], len(result)))
