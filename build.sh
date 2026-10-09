#!/bin/sh
# Diddy Kong Racing for the Nintendo 3DS - builder, for Linux and macOS.
#
#   ./build.sh                    with the ROM in the "rom" folder
#   ./build.sh path/to/the/rom
#
# Needs python3 and nothing else. The result appears in output/.
cd "$(dirname "$0")" || exit 1
command -v python3 > /dev/null || { echo "python3 is needed (any version from 3.6)"; exit 1; }
exec python3 builder/build.py "$@"
