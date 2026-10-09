#!/usr/bin/env bash
# Several consoles in one multiplayer session: copies of the portable Azahar
# on this PC. Each console's script works the Multiplayer screen itself: the
# first hosts, the others join.
#
#   tools/run-localplay.sh <host autotest> <joiner autotest> [seconds] [netlog]
#   PLAYERS=4 tools/run-localplay.sh ...        (2 by default; every joiner runs the joiner script)
#
# The scripts come from tools/make-lp-tests.py: lp-*.txt go through LOCAL
# WIRELESS (stood in for by UDP on the loopback address, lan.txt, see
# native/dkr-pc/3ds/link.c), on-*.txt through ONLINE and a game code.
#
# Collects out/localplay-run/: host.log, join.log (player 2), join3.log ...,
# a contact sheet of each console's screenshots, and compares the state
# checks the consoles logged (NETPLAY: poll N check X): they must agree.
# With `netlog` as the fourth argument every poll's check is logged, to find
# where they part (tools/netlog-diff.py reads two of the logs);
# NETLOG_WINDOW="from to" logs every object every poll between those polls.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
emu1=${DKR_EMULATOR:-$(ls -d "$root"/tools/azahar/azahar-windows-msvc-* 2>/dev/null | head -1)}
[ -d "$emu1" ] || { echo "no emulator: unpack a portable Azahar into tools/azahar/ (see docs/BUILDING.md) or set DKR_EMULATOR"; exit 1; }
players=${PLAYERS:-2}
seconds=${3:-120}
netlog=${4:-}
assets=${DKR_ASSETS:-$root/output/sdcard/3ds/DKR}
bottom=${DKR_BOTTOM:-$root/output/sdcard/3ds/DKR/bottom}

# Console n's emulator: tools/azahar for the host, tools/azaharN for the rest
# (copied from the first on first use).
emu_of() {
    if [ "$1" = 1 ]; then
        echo "$emu1"
    else
        echo "$root/tools/azahar$1/$(basename "$emu1")"
    fi
}
for n in $(seq 2 "$players"); do
    if [ ! -d "$(emu_of "$n")" ]; then
        mkdir -p "$root/tools/azahar$n"
        cp -r "$emu1" "$(emu_of "$n")"
        rm -rf "$(emu_of "$n")/user/sdmc/3ds/DKR"/shot_*.bmp
    fi
done
mkdir -p "$root/out"
# The program: the builder's by default (output/, from BUILD.cmd or
# builder/build.py). DKR_3DSX names another .3dsx file; DKR_BUILD a build
# directory under $HOME in WSL (the way the README builds on Windows).
if [ -n "${DKR_BUILD:-}" ]; then
    wsl -e bash -lc "cp \$HOME/$DKR_BUILD/dkracing.3dsx /mnt/c${root#/c}/out/DKR-native.3dsx"
else
    cp -f "${DKR_3DSX:-$root/output/sdcard/3ds/DKR/DKR.3dsx}" "$root/out/DKR-native.3dsx" || exit 1
fi

prepare() {    # prepare <emulator dir> <autotest> <host|join>
    local sd="$1/user/sdmc/3ds/DKR"
    mkdir -p "$sd"
    # DKR_CHARACTERS=out/characters: every console gets the assets with added
    # characters and their list (tools/import-characters.py), all unlocked in
    # a session; DKR_CHARACTERS_HOST_ONLY=1 gives the list to the host alone,
    # and the session must then do without them.
    local from="$assets"
    rm -f "$sd/characters.txt" "$sd/bananas.txt"
    if [ -n "${DKR_CHARACTERS:-}" ]; then
        from=$DKR_CHARACTERS
        if [ "$3" = host ] || [ -z "${DKR_CHARACTERS_HOST_ONLY:-}" ]; then
            cp -f "$from/characters.txt" "$sd/characters.txt"
            rm -rf "$sd/voices" "$sd/voices.bin"
            cp -rf "$from/voices" "$sd/voices"
        fi
    fi
    for f in assets.bin assets.lut.bin; do cmp -s "$from/$f" "$sd/$f" || cp -f "$from/$f" "$sd/$f"; done
    cp -f "$root/out/DKR-native.3dsx" "$sd/DKR-native.3dsx"
    cp -f "$2" "$sd/AUTOTEST.TXT"
    # The touch screen's pictures (the builder makes them).
    rm -rf "$sd/bottom"
    if [ -d "$bottom" ]; then mkdir -p "$sd/bottom"; cp -f "$bottom/"*.bin "$sd/bottom/"; fi
    rm -f "$sd"/log.txt "$sd"/shot_*.bmp "$sd"/bshot_*.bmp "$sd"/settings.ini "$sd"/netplay.txt "$sd"/NETLOG.TXT \
        "$sd"/lan.txt "$sd"/online.txt "$sd"/profile.txt
    # Local wireless is stood in for by UDP on the loopback address. Online
    # play runs as it is, except that the host is told its address (so the
    # router is left alone: noupnp) and the joiners are given the code that
    # address makes, there being no keyboard to type it on.
    printf 'port 6464\nhost 127.0.0.1\n' > "$sd/lan.txt"
    if [ "$3" = host ]; then
        printf 'port 6464\naddress 127.0.0.1\nnoupnp\n' > "$sd/online.txt"
    else
        printf 'noupnp\ncode %s\n' "$(python "$root/tools/make-lp-tests.py" code 127.0.0.1 6464)" > "$sd/online.txt"
    fi
    [ -n "$netlog" ] && echo "${NETLOG_WINDOW:-}" > "$sd/NETLOG.TXT"
    sed -i "s/^is_new_3ds=.*/is_new_3ds=true/" "$1/user/config/qt-config.ini"
}
prepare "$emu1" "$1" host
for n in $(seq 2 "$players"); do
    # JOIN3=<script>: player 3 runs its own script (one who joins later, say).
    script=$2
    [ "$n" = 3 ] && [ -n "${JOIN3:-}" ] && script=$JOIN3
    prepare "$(emu_of "$n")" "$script" join
done

for n in $(seq 1 "$players"); do
    e=$(emu_of "$n")
    (cd "$e" && ./azahar.exe "$(cygpath -w "$e/user/sdmc/3ds/DKR/DKR-native.3dsx")" > /dev/null 2>&1 &)
done
for i in $(seq 1 "$seconds"); do
    sleep 1
    grep -q "AUTOTEST: exit" "$emu1/user/sdmc/3ds/DKR/log.txt" 2>/dev/null && break
done
sleep 2
taskkill //IM azahar.exe //F > /dev/null 2>&1
for n in $(seq 1 "$players"); do
    sd="$(emu_of "$n")/user/sdmc/3ds/DKR"
    rm -f "$sd/lan.txt" "$sd/online.txt" "$sd/NETLOG.TXT" "$sd/AUTOTEST.TXT"
done

out="$root/out/localplay-run"
rm -rf "$out"; mkdir -p "$out"
name_of() {     # host, join, join3, join4 ...
    case "$1" in 1) echo host ;; 2) echo join ;; *) echo "join$1" ;; esac
}
dirs=()
for n in $(seq 1 "$players"); do
    sd="$(emu_of "$n")/user/sdmc/3ds/DKR"
    cp -f "$sd/log.txt" "$out/$(name_of "$n").log" 2>/dev/null
    dirs+=("$(name_of "$n")=$sd")
done
python - "$out" "${dirs[@]}" <<'PY'
import glob, os, sys
from PIL import Image
out = sys.argv[1]
for item in sys.argv[2:]:
    name, src = item.split("=", 1)
    shots = [Image.open(f).convert("RGB") for f in sorted(glob.glob(os.path.join(src, "shot_*.bmp")))]
    if shots:
        cols = 4
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new("RGB", (400 * cols, 240 * rows))
        for i, im in enumerate(shots):
            sheet.paste(im, ((i % cols) * 400, (i // cols) * 240))
        sheet.save(os.path.join(out, name + ".png"))
    print(name, len(shots), "shots")
    bottom = [Image.open(f).convert("RGB") for f in sorted(glob.glob(os.path.join(src, "bshot_*.bmp")))]
    for i, im in enumerate(bottom):
        im.save(os.path.join(out, "%s-bottom-%d.png" % (name, i)))
PY
grep -h "NETPLAY:\|LINK:" "$out/host.log" | grep -v "poll .* check" | head -10
grep "NETPLAY: poll" "$out/host.log" > "$out/host.checks"
for n in $(seq 2 "$players"); do
    name=$(name_of "$n")
    grep "NETPLAY: poll" "$out/$name.log" > "$out/$name.checks"
    common=$(comm -12 <(sort "$out/host.checks") <(sort "$out/$name.checks") | wc -l)
    echo "state checks: host $(wc -l < "$out/host.checks"), player $n $(wc -l < "$out/$name.checks"), identical $common"
done
grep -h "OUT OF STEP\|STALL\|CRASH" "$out"/*.log | sort -u | head -6
