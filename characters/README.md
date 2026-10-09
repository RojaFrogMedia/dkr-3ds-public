# The added characters

`added-characters.pack` adds 26 characters to the game's own ten. The
builder (`BUILD.cmd`, `build.sh`) puts them in by itself; there is nothing to
do here.

| | | | |
|---|---|---|---|
| Bomberman | Bottles | Bowser | Bubsy |
| Dixie Kong | Donkey Kong | Grunty | Jinjo |
| K. Rool | Kazooie | Laylee | Link |
| Luigi | Mario | Moggy | Mumbo Jumbo |
| Red Yoshi | Sabreman | Shovel Knight | Taj |
| Tiny Kong | Waluigi | Wario | Wizpig |
| Yooka | Yoshi | | |

Each has its own models (car, hovercraft, plane), its figure on the
character select, its portrait and its own voice. It drives like one of the
game's ten (its "donor") and has that one's engine sound.

## In the game

- **STORE** on the title screen lists them. Every banana you pick up in any
  race goes into a bank, and a character costs 150 bananas.
- On the character select, the ones you have bought stand to the right of
  the game's ten.
- In **multiplayer** all of them are open to everyone, as long as every
  console was made with the same pack (the game compares the lists and
  plays without them if they differ).
- The computer racers pick them too.

## Without them

Run the builder with `--no-characters` (`BUILD.cmd --no-characters` in a
terminal, or `./build.sh --no-characters`), or delete this folder. The game
is then the plain one with its ten characters.

## What the pack is

The characters come from character patches for the Nintendo 64 game
(`.xdelta` files), each of which replaces one of the ten racers.
`tools/import-characters.py` turned every patch into an additional
character, and `tools/make-character-pack.py` wrote the result as a
difference from the ROM: the pack holds the characters' models, textures,
animations, portraits and voices, and none of the game's own data.
`builder/characters.py` describes the file and puts the game's files
together from it and your ROM, checking each against the check sum the pack
carries.

To make your own from other patches, see "Added characters" in
`native/dkr-pc/3ds/README.md`.

## Credits

The characters are the work of the people who made the patches, not of this
project. The patches' file names credit **syeo** for Bomberman, Bottles,
Bowser, Donkey Kong, Grunty, Jinjo, Kazooie, Laylee, Luigi, Sabreman, Tiny
Kong, Waluigi and Wario. Link is from the patch "DKR OOT 64" and the two
Yoshis from "Yoshi Racing Story 64". The makers of the others are not named
in the files this was made from; if one of them is yours and you want your
name here, or the character taken out, open an issue.

The characters themselves belong to their owners (Nintendo, Rare, Konami,
Yacht Club Games, Playtonic and others). This is a fan project and has no
connection with any of them.
