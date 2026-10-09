#!/usr/bin/env bash
# Proves that the builder (builder/build.py, which uses no 3DS tools) writes
# the same files as makerom, bannertool and 3dsxtool do when they are given
# the builder's pictures. Linux or WSL, after tools/make-prebuilt.sh.
#
#   tools/check-builder.sh <dkracing.elf> <the ROM>
#
# Ends with "check-builder: ok" or says which file differs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
elf=${1:?usage: tools/check-builder.sh <dkracing.elf> <rom>}
rom=${2:?usage: tools/check-builder.sh <dkracing.elf> <rom>}
: "${DEVKITPRO:?DEVKITPRO is not set}"
dkp="$DEVKITPRO/tools/bin"
export PATH="${CIATOOLS:-$HOME/ciatools}/bin:$PATH"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$root/builder/build.py" "$rom" --out "$work/out" > "$work/build.log" || { cat "$work/build.log"; exit 1; }
built="$work/out/sdcard"
python3 "$root/tools/make-blank-art.py" "$work"

# The banner and the two icon files the builder made, taken back out.
python3 - "$root" "$built" "$work" <<'PY'
import os, sys
root, built, work = sys.argv[1:]
sys.path.insert(0, os.path.join(root, 'builder'))
import ctr
files = ctr.Cia(open(os.path.join(built, 'cias', 'DKR.cia'), 'rb').read()).program_files()
open(os.path.join(work, 'cia.smdh'), 'wb').write(files['icon'])
open(os.path.join(work, 'banner.bnr'), 'wb').write(files['banner'])
program = open(os.path.join(built, '3ds', 'DKR', 'DKR.3dsx'), 'rb').read()
at = int.from_bytes(program[0x20:0x24], 'little')
open(os.path.join(work, 'launcher.smdh'), 'wb').write(program[at:at + ctr.SMDH_BYTES])
PY

wrong=0
# 1. makerom, given that banner and icon, must write the builder's .cia.
#    makerom draws three numbers at random each time it runs (the ticket's
#    key and number, the content's number), so two runs of makerom itself
#    differ in those and in the signatures and check sums that cover them.
#    Everything else must be the same byte for byte, and the builder's own
#    check sums and signatures must be right.
makerom -f cia -o "$work/ref.cia" -elf "$elf" -rsf "$root/tools/cia/dkr.rsf" \
    -icon "$work/cia.smdh" -banner "$work/banner.bnr" -exefslogo -target t
python3 - "$root" "$work/ref.cia" "$built/cias/DKR.cia" <<'PY' || wrong=1
import os, sys
root, ref, built = sys.argv[1:]
sys.path.insert(0, os.path.join(root, 'builder'))
import ctr
a, b = (ctr.Cia(open(p, 'rb').read()) for p in (ref, built))


def fixed(part, holes):
    part = bytearray(part)
    for start, end in holes:
        part[start:end] = bytes(end - start)
    return bytes(part)


TICKET_RANDOM = ((4, 0x104), (0x1bf, 0x1d8))                    # signature; key and ticket number
TMD_RANDOM = ((4, 0x104), (0x140 + 0xa4, 0x140 + 0xc4),         # signature; check sum of the records' list
              (ctr.TMD_INFO_RECORDS + 4, ctr.TMD_INFO_RECORDS + 0x24),   # check sum of the content record
              (ctr.TMD_CHUNKS, ctr.TMD_CHUNKS + 4))             # the content's number
wrong = [name for name, same in (
    ('header', a.header == b.header), ('certificates', a.certs == b.certs),
    ('program', a.content == b.content), ('meta', a.meta == b.meta),
    ('ticket', fixed(a.ticket, TICKET_RANDOM) == fixed(b.ticket, TICKET_RANDOM)),
    ('TMD', fixed(a.tmd, TMD_RANDOM) == fixed(b.tmd, TMD_RANDOM))) if not same]
wrong += b.faults()
if wrong:
    print('DIFFERENT from makerom: DKR.cia (%s)' % ', '.join(wrong))
    sys.exit(1)
print('same as makerom: DKR.cia (program, header, certificates, meta; ticket and TMD but for makerom\'s random numbers)')
PY
# 2. 3dsxtool, given that icon file, must write the builder's .3dsx.
"$dkp/3dsxtool" "$elf" "$work/ref.3dsx" --smdh="$work/launcher.smdh"
cmp "$work/ref.3dsx" "$built/3ds/DKR/DKR.3dsx" && echo "same as 3dsxtool: DKR.3dsx" || wrong=1
# 3. bannertool, given the builder's pictures, must make the same banner
#    model and sound and the same icon pictures. (The packed bytes of the
#    model may differ: two packers, one content.)
bannertool makebanner -i "$work/out/pictures/banner.png" -a "$work/banner.wav" -o "$work/ref.bnr" > /dev/null
bannertool makesmdh -s DKR -l "Diddy Kong Racing" -p "dkr-3ds port" -i "$work/out/pictures/icon.png" \
    -f "visible,nosavebackups" -o "$work/ref.smdh" > /dev/null
python3 - "$root" "$work" <<'PY' || wrong=1
import os, sys
root, work = sys.argv[1:]
sys.path.insert(0, os.path.join(root, 'builder'))
import ctr
read = lambda name: open(os.path.join(work, name), 'rb').read()
ok = True
if ctr.banner_parts(read('banner.bnr')) == ctr.banner_parts(read('ref.bnr')):
    print('same as bannertool: the banner\'s model and sound')
else:
    print('DIFFERENT from bannertool: the banner'); ok = False
if read('cia.smdh') == read('ref.smdh'):
    print('same as bannertool: the icon file')
else:
    print('DIFFERENT from bannertool: the icon file'); ok = False
sys.exit(0 if ok else 1)
PY
[ "$wrong" = 0 ] && echo "check-builder: ok" || { echo "check-builder: something differs"; exit 1; }
