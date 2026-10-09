#!/usr/bin/env bash
# Play every character's every voice line in the emulator, record it and check
# it against the pack (tools/play-voices.py has the how and why).
#
#   tools/play-voices.sh [script numbers, e.g. 01 03]
#   DKR_BUILD=dkr-voices-build tools/play-voices.sh     (another build directory under $HOME in WSL)
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/out/play-voices"
python "$root/tools/play-voices.py" make || exit 1
scripts=("$@")
[ ${#scripts[@]} -eq 0 ] && scripts=($(cd "$out" && ls [0-9][0-9].txt | sed 's/\.txt$//'))
for n in "${scripts[@]}"; do
    frames=$(awk '$2 == "EXIT" { print $1 }' "$out/$n.txt")
    echo "== $n: $((frames / 60)) s of play"
    DKR_AUDIODUMP=1 DKR_CHARACTERS=${DKR_CHARACTERS:-out/characters} DKR_BANANAS=11500 \
        "$root/tools/run-native.sh" "$out/$n.txt" $((frames / 60 * 2 + 120)) | tail -1
    cp -f "$root/out/native-run/log.txt" "$out/$n.log"
    mv -f "$root/out/native-run/audio.raw" "$out/$n.raw" 2>/dev/null || echo "   no recording"
    grep -q "AUTOTEST: exit" "$out/$n.log" || echo "   the run did not reach its end"
done
python "$root/tools/play-voices.py" check
