# Building it yourself

You do not need any of this to play: `BUILD.cmd` (or `build.sh`) and a ROM
are enough, because the program is already compiled in `builder/prebuilt/`.
This page is for changing the game or checking that the compiled program is
what the source says.

## The whole chain

```
native/dkr-pc/  --make-->  dkracing.elf  --tools/make-prebuilt.sh-->  builder/prebuilt/DKR.3dsx
(source)                   (program)        (3dsxtool, makerom,                        DKR.cia
                                             bannertool)                (blank icon and banner)

your ROM  --builder/build.py-->  output/sdcard/3ds/DKR/assets.bin, assets.lut.bin   (cut out of the ROM,
                                 output/sdcard/3ds/DKR/characters.txt, voices/       and the added characters)
                                 output/sdcard/3ds/DKR/bottom/*.bin                 (pictures from the ROM)
                                 output/sdcard/3ds/DKR/DKR.3dsx                     (prebuilt + icon)
                                 output/sdcard/cias/DKR.cia                         (prebuilt + icon and banner)
```

The first row needs a 3DS toolchain and no ROM. The second needs the ROM
and nothing but Python. Nothing made from the ROM is ever needed to compile,
and nothing compiled contains anything from the ROM's data.

## 1. Compile the program

On Linux, or on Windows inside WSL. Install devkitPro's 3DS packages
([devkitpro.org/wiki/Getting_Started](https://devkitpro.org/wiki/Getting_Started)):

```sh
sudo dkp-pacman -S 3ds-dev          # devkitARM, libctru, citro3d, the 3DS tools
export DEVKITPRO=/opt/devkitpro
```

The source is C23 (`-std=gnu23`), so devkitARM must be recent; the shipped
program was compiled with devkitARM's GCC 16.1.0.

```sh
cd native/dkr-pc
make -f Makefile.3ds -j8            # build/3ds/dkracing.elf and dkracing.3dsx
```

`BUILD=<folder>` puts the build elsewhere (on WSL, a folder under `$HOME` is
much faster than one under `/mnt/c`). `TRACE=1` adds a call-stack tracer for
freeze reports and `PROFILE=1` times the renderer's parts; give each its own
`BUILD` folder. `native/dkr-pc/3ds/README.md` says what they print.

`include/asset_enums.h` is a list of names (levels, objects, sounds) that
the decompilation's asset tool generates. It is included here ready-made so
that compiling needs neither that tool nor a ROM.

The `dkracing.3dsx` that `make` writes already runs from the Homebrew
Launcher, with a default icon, given `assets.bin` and `assets.lut.bin` in
`sdmc:/3ds/DKR/`.

## 2. Package it

```sh
tools/make-prebuilt.sh native/dkr-pc/build/3ds/dkracing.elf
```

writes `builder/prebuilt/DKR.3dsx` and `DKR.cia` with a blank icon and an
empty banner picture. It needs `makerom` and `bannertool`, which devkitPro
does not package: if they are not on the `PATH` the script builds them from
their release tags into `$HOME/ciatools` (needs git, make, cmake and a C++
compiler). The title's description for makerom is `tools/cia/dkr.rsf`
(title `000400000FDD6400`; it asks for the New 3DS's 804 MHz clock and L2
cache, which is why an installed title is the build to use on a New 3DS).

Then run the builder as in the README. It puts the icon and banner it makes
from the ROM into those two files.

## 3. Check the builder against the real tools

The builder writes 3DS files without any 3DS tool: it replaces the icon
file inside the `.3dsx`, and inside the `.cia` the banner and the icon,
then works out again every check sum that covers them and signs again with
the same public test key makerom uses (`builder/testkey.py` says where it
comes from). To prove that this gives what the tools themselves would:

```sh
tools/check-builder.sh native/dkr-pc/build/3ds/dkracing.elf path/to/rom.z64
```

runs makerom, 3dsxtool and bannertool on the builder's pictures and compares:

```
same as makerom: DKR.cia (program, header, certificates, meta; ticket and TMD but for makerom's random numbers)
same as 3dsxtool: DKR.3dsx
same as bannertool: the banner's model and sound
same as bannertool: the icon file
check-builder: ok
```

(makerom draws three random numbers every time it runs, so two runs of
makerom itself never give the same `.cia`; those and the check sums over
them are the only bytes left out of the comparison.)

`python tools/check-cia.py` reads a `.cia` back: title, names, what it asks
of a New 3DS, every check sum and signature, and it writes the icon and the
banner's picture as PNG files to look at.

## How the builder works

| File | What it does |
|---|---|
| `builder/build.py` | The steps, in order, and the messages |
| `builder/rom.py` | Reads the ROM in any byte order, checks it is the US 1.0 release, finds the asset directory, decodes textures, fonts and sprites |
| `builder/characters.py` | Reads `characters/added-characters.pack` and puts the game's files with the added characters together from it and the ROM |
| `builder/picture.py` | Makes the pictures: the icon (DKR in the game's big font), the banner and touch-screen logo (the title screen's logo, put together from its eleven strips as the game does), the banana, the sky |
| `builder/ctr.py` | The 3DS formats: icon file, banner, `.3dsx`, `.cia`, signatures |
| `builder/testkey.py` | makerom's test key |

Without the added characters (`--no-characters`), `assets.bin` is not
converted in any way: it is the stretch of the ROM from `0xECC30` on,
10,340,864 bytes, and `assets.lut.bin` the 208 bytes before it. The game
does the byte-order work when it reads them.

## The added characters' pack

With the added characters, `assets.bin` is that same stretch with the
characters' models, textures, animations, portraits and sound bank entries
added as new records (13,418,112 bytes), and beside it go `characters.txt`
(the list) and `voices/` (a file of voice recordings per character).
`native/dkr-pc/3ds/README.md`, "Added characters", says how the game uses
them.

Those files are made from character patches by `tools/import-characters.py`
and are mostly the ROM's own data, so they are not in the repository. What
is, `characters/added-characters.pack`, is their difference from the ROM:
per file a list of steps, "copy so many bytes from this place in the ROM" or
"take the next so many bytes of the pack" (layout in
`builder/characters.py`). To make one from a folder of patches:

```sh
python tools/import-characters.py --rom path/to/rom.z64 --patches "folder of .xdelta files" --out out/characters
python tools/make-character-pack.py --rom path/to/rom.z64 --characters out/characters
```

The second reads the pack back, compares the files that come out with the
folder, and searches the pack's own bytes for anything of the ROM's. Consoles
that play together must be made with the same pack: the first line of
`characters.txt` carries a number that identifies the set, and a session
between consoles whose numbers differ plays without added characters.

## Running the tests in an emulator

The test runners are written for Windows with Git Bash:

1. Unpack a portable [Azahar](https://azahar-emu.org) into `tools/azahar/`
   so that `tools/azahar/azahar-windows-msvc-<version>/azahar.exe` exists,
   with a `user` folder beside it (which makes it portable). Start it once
   so that it writes its settings. Or point `DKR_EMULATOR` at such a folder.
2. `pip install pillow` (the runners paste the screenshots into one sheet).
3. Run the builder, then:

```sh
tools/run-native.sh native/tests/attract.txt 120            # the builder's DKR.3dsx
tools/run-native.sh native/tests/attract.txt 120 --old3ds   # with the emulator set to Old 3DS
DKR_BUILD=dkr-native-build tools/run-native.sh ...          # a build in $HOME/dkr-native-build in WSL
python tools/check-cia.py --cxi out/menu/DKR.cxi
DKR_RUN="$PWD/out/menu/DKR.cxi" tools/run-native.sh native/tests/attract.txt 120   # the .cia's program
```

A run is without the added characters unless it asks for them, with all of
them bought:

```sh
DKR_CHARACTERS=output/sdcard/3ds/DKR DKR_BANANAS=20000 DKR_PURCHASED=ffffffff \
    tools/run-native.sh native/tests/addedrace.txt 240
```

A run leaves `out/native-run/`: `log.txt`, `sheet.png` (top screen) and
`bottom.png` (touch screen). The tests are plain text, one key press per
line; the format is at the top of `native/dkr-pc/3ds/autotest.c`.
`native/tests/long.txt` is an eleven-minute soak through every demo level.

Multiplayer, with two to four emulators on one PC:

```sh
python tools/make-lp-tests.py
tools/run-localplay.sh native/tests/lp-host.txt native/tests/lp-join.txt 300
```

and it prints whether the consoles' state checks agree.
`native/dkr-pc/3ds/README.md` lists the other pairs of scripts.

Remember what an emulator is not: it runs at the Old 3DS's clock but models
no caches and runs the cores one at a time, so it flatters speed and hides
races between threads, and it has no HOME Menu.

## Changing the game

`native/dkr-pc/3ds/README.md` is the map: how a frame happens, which file
does what, how to read the log, the rules that keep it working (what may
run on which thread, what must be identical between consoles in a
multiplayer session), and where the eight-player work stands. Changes to
the game's own code for the 3DS are inside `#ifdef TARGET_3DS`.

`native/dkr-pc` is github.com/dfchil/Diddy-Kong-Racing as of its commit
`81171a2d` (2026-09-29) with the 3DS target added: the folder `3ds/`,
`Makefile.3ds`, and changes in `src/`, `libultra/` and `include/` that a
search for `TARGET_3DS` lists. It still holds that port's PC (Linux) and
Dreamcast targets and the decompilation's own N64 build; their instructions
are in `native/dkr-pc/README.md`.
