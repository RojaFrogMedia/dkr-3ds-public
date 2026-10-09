"""Read and write the adventure games in a Diddy Kong Racing save (EEPROM, 512 bytes).

    python tools/dkr_save.py <eeprom.bin>          print the three games

A game is 40 bytes of bits (unpack/pack below follow populate_settings_from_
save_data and func_800732E8 in native/dkr-pc/src/save_data.c):

    16  check: 5 + the sum of the other 38 bytes
    68  two bits for each race, challenge and boss level, in level order:
        0 never entered, 1 entered, 2 won, 3 won again (races: the silver coins)
     6  Taj's challenges
    10  trophies: two bits a world (0 none .. 3 gold)
    12  bosses: one bit each for the first and the second win
    42  golden balloons won in each of the six worlds (7 bits each; 0 is the island)
     3  pieces of the T.T. amulet,  3  pieces of the Wizpig amulet
    96  flags of each world (16 each): doors opened and the like
     8  keys found
    32  cutscenes seen
    16  the game's name, 3 letters packed
     8  nothing

Which levels have the two bits is read from the game's level headers, so
this needs the game's assets (assets.bin / assets.lut.bin).
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'output', 'sdcard', '3ds', 'DKR')     # the builder's
GAME_BYTES, WORLDS = 40, 6
FIELDS = (('taj', 6), ('trophies', 10), ('bosses', 12))


def level_types(assets=ASSETS):
    """(race type, world) of every level, from the level headers (asset sections 22 and 23)."""
    lut = open(os.path.join(assets, 'assets.lut.bin'), 'rb').read()
    data = open(os.path.join(assets, 'assets.bin'), 'rb').read()
    offsets = struct.unpack('>52I', lut)[1:]
    table = data[offsets[22]:offsets[23]]
    headers = data[offsets[23]:offsets[24]]
    starts = []
    for i in range(0, len(table), 4):
        value = struct.unpack_from('>I', table, i)[0]
        if value == 0xffffffff:
            break
        starts.append(value)
    return [(headers[s + 0x4C], struct.unpack_from('b', headers, s)[0]) for s in starts[:-1]]


def counted(race_type):
    return race_type == 0 or race_type & 0x40 or race_type == 8


class Bits:
    def __init__(self, data=b''):
        self.bits = ''.join('{:08b}'.format(b) for b in data)
        self.at = 0

    def take(self, count):
        value = int(self.bits[self.at:self.at + count] or '0', 2)
        self.at += count
        return value

    def put(self, count, value):
        if count > 0:
            self.bits += '{:0{}b}'.format(value & ((1 << count) - 1), count)


def unpack(game, types):
    """A game's 40 bytes as a dict; None if it is unused or damaged."""
    if 5 + sum(game[2:]) & 0xffff != struct.unpack('>H', game[:2])[0]:
        return None
    bits = Bits(game)
    bits.take(16)
    save = {'levels': {}}
    used = 0
    for level, (race_type, _) in enumerate(types):
        if counted(race_type):
            save['levels'][level] = bits.take(2)
            used += 2
    bits.take(68 - used)
    for name, size in FIELDS:
        save[name] = bits.take(size)
    save['balloons'] = [bits.take(7) for _ in range(WORLDS)]
    save['tt_amulet'] = bits.take(3)
    save['wizpig_amulet'] = bits.take(3)
    save['world_flags'] = [bits.take(16) for _ in range(WORLDS)]
    save['keys'] = bits.take(8)
    save['cutscenes'] = bits.take(32)
    save['name'] = bits.take(16)
    return save


def pack(save, types):
    bits = Bits()
    bits.put(16, 0)
    used = 0
    for level, (race_type, _) in enumerate(types):
        if counted(race_type):
            bits.put(2, save['levels'].get(level, 0))
            used += 2
    bits.put(68 - used, 0)
    for name, size in FIELDS:
        bits.put(size, save[name])
    for count in save['balloons']:
        bits.put(7, count)
    bits.put(3, save['tt_amulet'])
    bits.put(3, save['wizpig_amulet'])
    for flags in save['world_flags']:
        bits.put(16, flags)
    bits.put(8, save['keys'])
    bits.put(32, save['cutscenes'])
    bits.put(16, save['name'])
    bits.put(8, 0)
    game = bytearray(int(bits.bits[i:i + 8], 2) for i in range(0, GAME_BYTES * 8, 8))
    struct.pack_into('>H', game, 0, (5 + sum(game[2:])) & 0xffff)
    return bytes(game)


def describe(save, types):
    lines = ['  levels   ' + ' '.join('%d:%d' % item for item in sorted(save['levels'].items()))]
    lines.append('  taj {:06b}  trophies {:010b}  bosses {:012b}'.format(save['taj'], save['trophies'], save['bosses']))
    lines.append('  balloons %s (%d)  T.T. amulet %d  Wizpig amulet %d' %
                 (save['balloons'], sum(save['balloons']), save['tt_amulet'], save['wizpig_amulet']))
    lines.append('  world flags {}  keys {:08b}  cutscenes {:08x}  name {:04x}'.format(
        ' '.join('%04x' % f for f in save['world_flags']), save['keys'], save['cutscenes'], save['name']))
    return '\n'.join(lines)


if __name__ == '__main__':
    types = level_types()
    data = open(sys.argv[1], 'rb').read()
    for number in range(3):
        game = data[number * GAME_BYTES:(number + 1) * GAME_BYTES]
        save = unpack(game, types)
        print('GAME %s%s' % ('ABC'[number], '' if save else ': unused'))
        if save:
            assert pack(save, types) == game, 'pack(unpack()) differs'
            print(describe(save, types))
