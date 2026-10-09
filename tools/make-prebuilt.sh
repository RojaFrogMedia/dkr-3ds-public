#!/usr/bin/env bash
# Packs a build of the game into builder/prebuilt/: DKR.3dsx (Homebrew
# Launcher) and DKR.cia (HOME Menu), both with blank pictures. The builder
# (BUILD.cmd, builder/build.py) then only has to put in the pictures it makes
# from the ROM. Linux or WSL; see docs/BUILDING.md.
#
#   tools/make-prebuilt.sh <dkracing.elf>
#
# Needs devkitPro's 3DS tools (DEVKITPRO set: smdhtool, 3dsxtool), python3,
# and makerom and bannertool. The last two are not devkitPro packages: if
# they are not on the PATH or in $CIATOOLS/bin they are built from their
# release tags into $CIATOOLS (default $HOME/ciatools; needs git, make, cmake
# and a C++ compiler).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
elf=${1:?usage: tools/make-prebuilt.sh <dkracing.elf>}
[ -f "$elf" ] || { echo "no such file: $elf"; exit 1; }
: "${DEVKITPRO:?DEVKITPRO is not set}"
dkp="$DEVKITPRO/tools/bin"
tools=${CIATOOLS:-$HOME/ciatools}
export PATH="$tools/bin:$PATH"

# What the menus show, and the title's number on the console
# (000400000FDD6400; the same names are in tools/cia/dkr.rsf).
SHORT="DKR"
LONG="Diddy Kong Racing"
AUTHOR="dkr-3ds port"

mkdir -p "$tools/bin"
if ! command -v makerom > /dev/null; then
    rm -rf "$tools/makerom"
    git clone -q --depth 1 --branch makerom-v0.19.0 https://github.com/3DSGuy/Project_CTR.git "$tools/makerom"
    make -C "$tools/makerom/makerom" -j4 deps > "$tools/makerom-build.log" 2>&1
    make -C "$tools/makerom/makerom" -j4 >> "$tools/makerom-build.log" 2>&1
    cp "$tools/makerom/makerom/bin/makerom" "$tools/bin/"
fi
if ! command -v bannertool > /dev/null; then
    rm -rf "$tools/bannertool"
    git clone -q --depth 1 --branch 1.2.3 https://github.com/carstene1ns/3ds-bannertool.git "$tools/bannertool"
    cmake -S "$tools/bannertool" -B "$tools/bannertool/build" -DCMAKE_BUILD_TYPE=Release > "$tools/bannertool-build.log" 2>&1
    cmake --build "$tools/bannertool/build" -j4 >> "$tools/bannertool-build.log" 2>&1
    cp "$tools/bannertool/build/bannertool" "$tools/bin/"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/builder/prebuilt"
mkdir -p "$out"
python3 "$root/tools/make-blank-art.py" "$work"

# The Homebrew Launcher's build: the .3dsx with an icon file inside it.
"$dkp/smdhtool" --create "$SHORT" "$LONG" "$AUTHOR" "$work/icon.png" "$work/launcher.smdh"
"$dkp/3dsxtool" "$elf" "$out/DKR.3dsx" --smdh="$work/launcher.smdh"

# The HOME Menu's build: the same program as an installable title, with the
# flat banner most homebrew has (a picture on a panel, and a chime).
bannertool makesmdh -s "$SHORT" -l "$LONG" -p "$AUTHOR" -i "$work/icon.png" \
    -f "visible,nosavebackups" -o "$work/cia.smdh" > /dev/null
bannertool makebanner -i "$work/banner.png" -a "$work/banner.wav" -o "$work/banner.bnr" > /dev/null
makerom -f cia -o "$out/DKR.cia" -elf "$elf" -rsf "$root/tools/cia/dkr.rsf" \
    -icon "$work/cia.smdh" -banner "$work/banner.bnr" -exefslogo -target t

ls -l "$out/DKR.3dsx" "$out/DKR.cia"
