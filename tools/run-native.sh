#!/usr/bin/env bash
# Run the game in a portable Azahar under tools/azahar (Windows, Git Bash).
#
#   tools/run-native.sh <autotest script> [seconds] [--old3ds]
#   DKR_BUILD=dkr-trace-build tools/run-native.sh ...   (a build directory under $HOME in WSL)
#
# Copies the .3dsx and the asset files (by default the builder's, from
# output/sdcard/3ds/DKR) and the script as AUTOTEST.TXT (format in
# native/dkr-pc/3ds/autotest.c), then collects out/native-run/: log.txt, the
# screenshots as PNG and a contact sheet (bottom.png for the touch screen).
# settings.ini is removed first so every run starts from the defaults.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
emu=${DKR_EMULATOR:-$(ls -d "$root"/tools/azahar/azahar-windows-msvc-* 2>/dev/null | head -1)}
[ -d "$emu" ] || { echo "no emulator: unpack a portable Azahar into tools/azahar/ (see docs/BUILDING.md) or set DKR_EMULATOR"; exit 1; }
sd="$emu/user/sdmc/3ds/DKR"
script=${1:?autotest script}
seconds=${2:-90}
new=true; [ "${3:-}" = "--old3ds" ] && new=false
assets=${DKR_ASSETS:-$root/output/sdcard/3ds/DKR}
bottom=${DKR_BOTTOM:-$root/output/sdcard/3ds/DKR/bottom}

mkdir -p "$sd" "$root/out"
# The program: the builder's by default (output/, from BUILD.cmd or
# builder/build.py). DKR_3DSX names another .3dsx file; DKR_BUILD a build
# directory under $HOME in WSL (the way the README builds on Windows).
if [ -n "${DKR_BUILD:-}" ]; then
    wsl -e bash -lc "cp \$HOME/$DKR_BUILD/dkracing.3dsx /mnt/c${root#/c}/out/DKR-native.3dsx"
else
    cp -f "${DKR_3DSX:-$root/output/sdcard/3ds/DKR/DKR.3dsx}" "$root/out/DKR-native.3dsx" || exit 1
fi
# A run is without the added characters (their list is left out, so the game
# is the plain one whatever the asset file holds) unless it asks for them:
# DKR_CHARACTERS=output/sdcard/3ds/DKR, the builder's, or another folder of
# tools/import-characters.py; DKR_BANANAS=1500: the banana bank to start with.
rm -f "$sd/characters.txt" "$sd/bananas.txt"
if [ -n "${DKR_CHARACTERS:-}" ]; then
    assets=$DKR_CHARACTERS
    cp -f "$assets/characters.txt" "$sd/characters.txt"
    # Each added character's voice file (voices/<character>.bin), and nothing of an older pack.
    rm -rf "$sd/voices" "$sd/voices.bin"
    cp -rf "$assets/voices" "$sd/voices"
    # DKR_PURCHASED=ffffffff: which added characters are owned, one bit each (bit 0 the first).
    [ -n "${DKR_BANANAS:-}" ] && printf 'bananas %s
purchased %s
' "$DKR_BANANAS" "${DKR_PURCHASED:-0}" > "$sd/bananas.txt"
fi
for f in assets.bin assets.lut.bin; do cmp -s "$assets/$f" "$sd/$f" || cp -f "$assets/$f" "$sd/$f"; done
cp -f "$root/out/DKR-native.3dsx" "$sd/DKR-native.3dsx"
cp -f "$script" "$sd/AUTOTEST.TXT"
# The touch screen's pictures (the builder makes them); DKR_NO_BOTTOM_ART=1
# runs without, to see the text that stands in.
rm -rf "$sd/bottom"
if [ -d "$bottom" ] && [ -z "${DKR_NO_BOTTOM_ART:-}" ]; then
    mkdir -p "$sd/bottom"; cp -f "$bottom/"*.bin "$sd/bottom/"
fi
rm -f "$sd"/log.txt "$sd"/shot_*.bmp "$sd"/bshot_*.bmp "$sd"/settings.ini "$emu/user/log/azahar_log.txt"
# DKR_SETTINGS="save_file=1": a line for settings.ini (the touch screen menu's file).
[ -n "${DKR_SETTINGS:-}" ] && echo "$DKR_SETTINGS" > "$sd/settings.ini"
# DKR_AUDIODUMP=1: the game records what it mixes to audio.raw (collected below).
rm -f "$sd/AUDIODUMP.TXT" "$sd/audio.raw"
[ -n "${DKR_AUDIODUMP:-}" ] && touch "$sd/AUDIODUMP.TXT"
# (The second line matters: while the emulator's "default" mark is true it
# ignores the value and emulates a New 3DS.)
sed -i -e "s/^is_new_3ds=.*/is_new_3ds=$new/" -e 's/^is_new_3ds\\default=.*/is_new_3ds\\default=false/' \
    "$emu/user/config/qt-config.ini"

# DKR_RUN=out/menu/DKR.cxi: start this file instead (the installed title out of
# the .cia, from tools/check-cia.py --cxi); the SD card is set up the same.
run=${DKR_RUN:-$sd/DKR-native.3dsx}
(cd "$emu" && ./azahar.exe "$(cygpath -w "$run")" > /dev/null 2>&1 &)
for i in $(seq 1 "$seconds"); do
    sleep 1
    grep -q "AUTOTEST: exit" "$sd/log.txt" 2>/dev/null && break
    tasklist //FI "IMAGENAME eq azahar.exe" 2>/dev/null | grep -q azahar || { echo "emulator exited after ${i}s"; break; }
done
sleep 1
taskkill //IM azahar.exe //F > /dev/null 2>&1

out="$root/out/native-run"
rm -rf "$out"; mkdir -p "$out"
cp -f "$sd/log.txt" "$out/log.txt" 2>/dev/null
[ -f "$sd/audio.raw" ] && mv -f "$sd/audio.raw" "$out/audio.raw"
cp -f "$emu/user/log/azahar_log.txt" "$out/azahar_log.txt" 2>/dev/null
python - "$sd" "$out" <<'PY'
import glob, os, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
shots = []
for i, f in enumerate(sorted(glob.glob(os.path.join(src, "shot_*.bmp")))):
    try:
        shots.append(Image.open(f).convert("RGB"))
    except Exception as e:
        print("bad shot", f, e)
if shots:
    cols = 4
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (400 * cols, 240 * rows))
    for i, im in enumerate(shots):
        sheet.paste(im, ((i % cols) * 400, (i // cols) * 240))
    sheet.save(os.path.join(dst, "sheet.png"))
print(len(shots), "shots")
bottom = [Image.open(f).convert("RGB") for f in sorted(glob.glob(os.path.join(src, "bshot_*.bmp")))]
if bottom:
    cols = 4
    rows = (len(bottom) + cols - 1) // cols
    sheet = Image.new("RGB", (320 * cols, 240 * rows))
    for i, im in enumerate(bottom):
        sheet.paste(im, ((i % cols) * 320, (i // cols) * 240))
    sheet.save(os.path.join(dst, "bottom.png"))
    print(len(bottom), "bottom screen shots")
PY
echo "log lines: $(wc -l < "$out/log.txt" 2>/dev/null)"
