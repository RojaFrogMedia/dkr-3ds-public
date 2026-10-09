# Diddy Kong Racing for the Nintendo 3DS

A native port of Diddy Kong Racing (Nintendo 64, 1997) to the Nintendo 3DS:
the game's decompiled source code compiled for the console itself, not an
emulator. Widescreen on the top screen, 60 frames a second, the race's
standings and map on the touch screen, multiplayer between consoles, and 26
added characters beside the game's ten.

**No game data is included.** You supply your own ROM; the builder in this
folder takes the game's data out of it and gives you a `.3dsx` and a `.cia`
ready for the SD card.

**To get it:** on GitHub, press the green **Code** button and choose
**Download ZIP**, then unpack the ZIP anywhere. (Or clone the repository.)

This is an unofficial fan project, not affiliated with or endorsed by
Nintendo or Rare.

## What you need

- A Nintendo 3DS family console (New 3DS / New 2DS XL recommended) with
  custom firmware and the Homebrew Launcher. [3ds.hacks.guide](https://3ds.hacks.guide)
  explains how; this project does not.
- Your own ROM of **Diddy Kong Racing (USA) (En,Fr), version 1.0**, the
  first US release.
  SHA-1 `0cb115d8716dbbc2922fda38e533b9fe63bb9670` (as a `.z64` file).
  Other releases (Europe, Japan, US 1.1) and modified ROMs are refused.
- A PC to run the builder once: Windows, Linux or macOS.

- Custom Character credits:
#   Character       Credit   Source patch
---  --------------  -------  ------------------------------------------------------------
 1   Bomberman       syeo     bomberman - syeo (recomp).xdelta
 2   Bottles         syeo     bottles - syeo.xdelta
 3   Bowser          syeo     bowser - syeo.xdelta
 4   Dixie Kong      syeo         dixie kong.xdelta
 5   Link            syeo         DKR-OOT-64-1.1.xdelta ("DKR OOT 64")
 6   Yoshi           ThatGuyMcd         DKR-Yoshi-Racing-Story64v04.xdelta ("Yoshi Racing Story 64")
 7   Red Yoshi       ThatGuyMcd         DKR-Yoshi-Racing-Story64v04.xdelta ("Yoshi Racing Story 64")
 8   Donkey Kong     syeo     donkey kong - syeo.xdelta
 9   Grunty          syeo     grunty - syeo (recomp).xdelta
10   Jinjo           syeo     jinjo (blue) - syeo.xdelta
11   Kazooie         syeo     kazooie - syeo.xdelta
12   K. Rool         syeo         kkrool.xdelta
13   Laylee          syeo     laylee - syeo.xdelta
14   Luigi           syeo     luigi - syeo.xdelta
15   Mario           syeo         mario.xdelta
16   Sabreman        syeo     sabreman - syeo.xdelta
17   Shovel Knight   syeo         shovelknight.xdelta
18   Taj             syeo         taj.xdelta
19   Tiny Kong       syeo     tiny kong - syeo.xdelta
20   Waluigi         syeo     waluigi - syeo.xdelta
21   Wario           syeo     wario - syeo (recomp).xdelta
22   Wizpig          ThatGuyMcd         wizpig.xdelta
23   Yooka           syeo         yooka.xdelta
24   Bubsy           That Smol Bobcat         (no patch file on this PC)
25   Moggy           syeo         (no patch file on this PC)
26   Mumbo Jumbo     syeo         (no patch file on this PC)

  

## Making the game: three steps

### 1. Put your ROM in the `rom` folder

Any of `.z64`, `.n64`, `.v64`, or a `.zip` with the ROM in it. The file's
name does not matter.

### 2. Run the builder

- **Windows:** double-click `BUILD.cmd`. (Or skip step 1 and drop the ROM
  file onto `BUILD.cmd`.)
- **Linux, macOS:** `./build.sh` in a terminal (or `./build.sh path/to/rom`).

It takes a few seconds and ends with `DONE`:

```
1/4  Checking the ROM
2/4  Taking the game's data out of the ROM
     with 26 added characters: Bomberman, Bottles, Bowser, ...
3/4  Making the pictures from the ROM's own
4/4  Writing the game
DONE. Everything is in: ...\output\sdcard
```

The builder is a small Python program (`builder/`). On Windows, if Python is
not installed, `BUILD.cmd` offers to download a private copy from python.org
(11 MB) into `builder\python`; nothing is installed on the PC. On Linux and
macOS it uses the `python3` that is already there. It needs no other
software and no internet connection.

If the builder stops, it says why in plain words: no ROM found, the wrong
release, a modified ROM.

The added characters are put in by default. For the game without them, run
`BUILD.cmd --no-characters` (or `./build.sh --no-characters`) in a terminal.

### 3. Copy the result to the SD card

`output/sdcard/` now holds two folders. Copy both onto the root of the
console's SD card, merging them with the `3ds` and `cias` folders that are
already there:

```
output/sdcard/
  3ds/DKR/DKR.3dsx           the game, for the Homebrew Launcher
  3ds/DKR/assets.bin         the game's data, from your ROM
  3ds/DKR/assets.lut.bin
  3ds/DKR/characters.txt     the added characters' list
  3ds/DKR/voices/            and their voices, a file each
  3ds/DKR/bottom/            pictures for the touch screen
  cias/DKR.cia               the game, to install on the HOME Menu
```

Then either:

- **Homebrew Launcher:** start *DKR* from the list. Nothing to install.
- **HOME Menu:** install `cias/DKR.cia` with FBI. The game then has its own
  icon on the HOME Menu. **On a New 3DS use this one:** an installed title is
  given the New 3DS's faster processor mode for certain, a `.3dsx` may not be.

Both are the same program and both read the files in `3ds/DKR/` on the SD
card, so that folder is needed either way. Saves, settings and the log are
kept there too.

**Sound** needs the console's DSP firmware file, `sdmc:/3ds/dspfirm.cdc`,
like most 3DS homebrew. If the game is silent, make it once: in the Rosalina
menu (L + Down + SELECT) choose *Miscellaneous options*, *Dump DSP firmware*.

## Controls

| 3DS | In the game |
|---|---|
| Circle Pad | steer |
| A | accelerate |
| B | brake / reverse |
| R | hop and slide |
| L or ZL | use item (the N64's Z) |
| X, Y, ZR | C-Up, C-Down, C-Right |
| C-Stick (New 3DS) or Circle Pad Pro (Old 3DS) | the four C buttons |
| D-pad | the N64's D-pad |
| START | pause |
| SELECT, or a tap on the touch screen | settings menu |
| HOME | pause; HOME again closes the game, any other button resumes |

Any button during the opening logos skips them. On the track select (Tracks
mode), L picks a random track.

The settings menu on the touch screen has: frame rate (auto, 60, 30),
volume, backlight, Circle Pad dead zone and range, C-Stick on/off, hard
mode, which save file to use, live frame statistics, and the controls.

## What is different from the N64 game

- **Widescreen** across the 400-pixel top screen; menus keep their shape in
  the middle.
- **60 frames a second** (the N64 game ran at 30 or less), dropping to 30
  only while a scene is too heavy.
- **The touch screen** shows, in a race, the standings with portraits, the
  track's map, speed, lap and bananas (the banana count and the map are
  taken off the top screen); in the Adventure's hub, what the save has
  collected; on menus, the logo.
- **Multiplayer between consoles** instead of split screen: a MULTIPLAYER
  entry on the title screen, LOCAL WIRELESS or ONLINE, up to four players,
  each with the whole screen. Every track and character is open there, and
  computer racers fill the grid. See below.
- **Hard mode** (settings menu): the computer racers are held to a schedule
  taken from T.T.'s ghost times.
- **A second save file** with everything unlocked, picked in the settings
  menu. Your own save is untouched by it.
- **26 added characters** beside the game's ten: Bomberman, Bottles, Bowser,
  Bubsy, Dixie Kong, Donkey Kong, Grunty, Jinjo, K. Rool, Kazooie, Laylee,
  Link, Luigi, Mario, Moggy, Mumbo Jumbo, Red Yoshi, Sabreman, Shovel Knight,
  Taj, Tiny Kong, Waluigi, Wario, Wizpig, Yooka and Yoshi, each with its own
  models, portrait and voice. Every banana picked up in a race goes into a
  bank, and STORE on the title screen sells a character for 150 bananas; in
  multiplayer all of them are open. `characters/README.md` has the rest,
  and how to build the game without them.

### Multiplayer, and what "online" does

- **Local wireless:** one console hosts, the others see it in a list.
- **Online:** there is no server. The host's console is the other end, and
  the twelve-character game code the host is shown **is the host's public
  internet address and port, written another way. Only give a code to
  people you would give your address to.** To let the others in, the game
  asks the host's router (UPnP) to forward UDP port 6464 to the console and
  removes the forwarding when the game ends; with UPnP off on the router,
  the port has to be forwarded by hand. `tools/upnp-port.py` shows or
  removes a forwarding left behind after a crash.

## How far it has been tested

Said plainly, so that you know what to expect:

- **Played on a real New 3DS:** the single-player game (Adventure, Tracks
  mode, menus, saving), at about 60 frames a second in most scenes.
- **Tested only in an emulator (Azahar):** this exact release build. The
  Old 3DS: in the emulator's Old 3DS mode the demo scenes hold 60 frames a
  second about two thirds of the time and fall to 30 in the heavier ones; a
  real Old 3DS has not been tried and may be slower. The added characters
  (the select, races, results, and every voice line played back and compared
  with its recording). Every multiplayer
  feature, with two to four emulated consoles: local wireless between real
  consoles and online play between two real networks have **not** been
  tried.
- **Not tried at all:** the Circle Pad Pro (written to its documented
  protocol only).
- **The `.cia`** made by the builder is checked to be the same file that
  the usual tools (makerom, bannertool) produce, and its program runs in the
  emulator; the HOME Menu itself cannot be run in an emulator, so the icon
  and banner on a real HOME Menu are checked by reading the file back, not
  by eye. The banner is the plain kind most homebrew uses.

Known gaps:

- The console's graphics chip drops a triangle that has a corner very far
  off the screen, which the emulator does not; now and then a large piece of
  scenery close to the camera can be missing for a moment.
- No Controller Pak, so no saved ghosts.
- Reports of the engine sound cutting out and of some item sounds missing in
  races have not been reproduced; the log's `AUDIO:` lines are the place to
  look.

## When something goes wrong

Everything the game keeps is in `sdmc:/3ds/DKR/`:

| File | What |
|---|---|
| `log.txt` | Written while the game runs: frames per second once a second, and a `CRASH` or `STALL` report if it dies. `log-prev.txt` is the run before, kept when that run did not end properly |
| `state.txt` | The last thing the game was doing; `ended` after a clean exit |
| `eeprom.bin` | Your save (the N64 cartridge's 512 bytes) |
| `settings.ini` | The settings menu's choices; delete it for the defaults |

| Symptom | Cause |
|---|---|
| The game closes at once or shows a black screen | `assets.bin` and `assets.lut.bin` are not in `sdmc:/3ds/DKR/`, or the copy was cut short. Copy the whole `3ds/DKR` folder again |
| No STORE on the title screen, or added characters look or sound wrong | `assets.bin`, `assets.lut.bin`, `characters.txt` and the `voices` folder belong together: copy all of `3ds/DKR` from one run of the builder, not some files from an older one |
| No sound at all | `sdmc:/3ds/dspfirm.cdc` is missing (see above) |
| Slow on a New 3DS from the Homebrew Launcher | install the `.cia` |
| The builder says the ROM is the wrong release or modified | it needs the unmodified US version 1.0; the message says what it found instead |

`native/dkr-pc/3ds/README.md` explains how to read the log ("Reading the
log", "When it freezes or crashes", "Sound that goes missing").

## What is in this folder

| Path | What |
|---|---|
| `BUILD.cmd`, `build.sh` | Run the builder (Windows; Linux and macOS) |
| `rom/` | Where your ROM goes. Empty |
| `characters/` | The added characters: one file, `added-characters.pack`, and its README |
| `builder/` | The builder: `build.py` and four small modules (reading the ROM, adding the characters, making the pictures, writing the 3DS files). `builder/prebuilt/` holds the game's program, already compiled, with blank pictures |
| `output/` | Made by the builder; not part of the repository, and not to be shared: it holds the game's data |
| `native/dkr-pc/` | **The game's source code**: the decompilation (`src/`, `libultra/`) and the Nintendo 3DS layer (`3ds/`). `3ds/README.md` explains how it works, file by file |
| `native/tests/` | Scripted test runs for the emulator |
| `tools/` | For developers: packaging from source, the test runners, the builder's cross-check, save and character tools |
| `docs/BUILDING.md` | Compiling the program yourself, and how the builder works |
| `docs/HOW-NATIVE-PC-PORTS-RUN-AT-60FPS-ON-3DS.txt` | How the 60 fps was reached |

The compiled program in `builder/prebuilt/` contains program code only. The
game's models, textures, levels, sound and text are not in it: they are in
`assets.bin`, which exists only once you have run the builder on your ROM.
The icon and the banner are likewise made from your ROM's own pictures and
are not shipped. `characters/added-characters.pack` holds only what the
added characters brought with them, written as a difference from the ROM,
and is of no use without one.

## Credits

- The Diddy Kong Racing decompilation and its contributors
  (github.com/DavidSM64/Diddy-Kong-Racing), whose source code this is.
- dfchil's port of that decompilation to PC and the Dreamcast
  (github.com/dfchil/Diddy-Kong-Racing), which the 3DS target was added to.
- The makers of the character patches the added characters come from;
  `characters/README.md` names those the files name.
- devkitPro: devkitARM, libctru, citro3d and the 3DS tools.
- makerom (3DSGuy, jakcron; Project_CTR) and bannertool (Steveice10,
  carstene1ns), which package the program in `builder/prebuilt/`.
- Azahar, the emulator the tests run in.

## Legal

Diddy Kong Racing is the property of Rare and Nintendo. This project
contains none of the game's data and does not help in obtaining it; it is
only useful to someone who already has the game. The added characters are
fan-made and belong to their makers and to the characters' owners
(`characters/README.md`). The decompiled source in
`native/dkr-pc/` is published by its authors under the terms in
`native/dkr-pc/LICENSE.md`; the files added for the 3DS follow the same
terms. Use it at your own risk: it comes with no warranty of any kind.
