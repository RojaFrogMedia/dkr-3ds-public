#!/usr/bin/env bash
# Every boss's race in the emulator, one run each, without playing up to them
# (native/tests/boss.txt and its BOSS command). Git Bash.
#
#   tools/run-bosses.sh                     all ten: the five bosses and their rematches
#   tools/run-bosses.sh tricky wizpig2      only these
#   tools/run-bosses.sh taj                 Taj's car race on the island
#   BOSS_TEST=native/tests/boss-added.txt DKR_CHARACTERS=output/sdcard/3ds/DKR \
#       DKR_BANANAS=20000 DKR_PURCHASED=ffffffff tools/run-bosses.sh tricky
#
# Leaves out/bosses/<boss>-top.png and <boss>-bottom.png (the screenshots of
# the two screens side by side) and <boss>-log.txt.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
bosses=("$@")
[ ${#bosses[@]} -eq 0 ] && bosses=(tricky bluey bubbler smokey wizpig tricky2 bluey2 bubbler2 smokey2 wizpig2)
test=${BOSS_TEST:-$root/native/tests/boss.txt}
out="$root/out/bosses"
mkdir -p "$out"
for boss in "${bosses[@]}"; do
    sed "s/BOSSNAME/$boss/" "$test" > "$out/script.txt"
    bash "$root/tools/run-native.sh" "$out/script.txt" 150
    cp -f "$root/out/native-run/sheet.png" "$out/$boss-top.png" 2>/dev/null
    cp -f "$root/out/native-run/bottom.png" "$out/$boss-bottom.png" 2>/dev/null
    cp -f "$root/out/native-run/log.txt" "$out/$boss-log.txt" 2>/dev/null
done
rm -f "$out/script.txt"
