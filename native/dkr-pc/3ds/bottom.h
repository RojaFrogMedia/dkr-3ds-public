// What the touch screen shows outside its settings menu (bottomscreen.c):
// in a race the standings, the map, speed, bananas and lap; in the Adventure
// hub what is still to be collected; anywhere else the logo and the banana
// bank. The game tells it what is going on through the snapshot below
// (3ds/bottom_game.inc, compiled into src/game_ui.c). Plain C types only: the
// game's headers and libctru's cannot be included together.
#ifndef DKR_3DS_BOTTOM_H
#define DKR_3DS_BOTTOM_H

#define BOTTOM_MAX_RACERS 8
#define BOTTOM_MAP_MAX 128          // the map sprite's largest side, in its own pixels
#define BOTTOM_TEXTURE_MAX 64       // a portrait's largest side

enum {
    BOTTOM_SCENE_NONE,      // menus, cutscenes, the attract demos: nothing with racers and a map
    BOTTOM_SCENE_RACE,      // a race, a boss or a challenge
    BOTTOM_SCENE_HUB,       // driving around the Adventure's island or a world's lobby
};

typedef struct {
    int place;              // 1 = leading
    int character;          // the game's ten, 0-9 (an added character's donor)
    int modCharacter;       // 3ds/characters.c: 0 for one of the game's own
    int player;             // -1: a computer racer; else the controller, which in a session is the player
    int finished;
    int mapX, mapY;         // on the map, in sixteenths of a map pixel
    unsigned char colour[3];
    const char *name;       // the character's
    // The portrait: 0xAARRGGBB, rows from the top; NULL when there is none.
    // portraitSerial changes when the picture does.
    const unsigned *portrait;
    int portraitWidth, portraitHeight;
    unsigned portraitSerial;
} BottomRacer;

typedef struct {
    unsigned frame;         // goes up each time the game fills this in; standing still means no race is shown
    int scene;              // BOTTOM_SCENE_*
    int racerCount;
    BottomRacer racers[BOTTOM_MAX_RACERS];
    int me;                 // this console's racer in `racers`, or -1
    int speed;              // what the game's own dial shows, 0 to 150
    int bananas;
    int lap, laps;          // laps 0: a race without laps (boss, challenge)
    int silverCoins;        // -1 unless this is a silver coin race; else 0 to 8
    int adventure;          // 1: an Adventure save is being played, `collect` counts
    int twoPlayer;          // two players on this console (no single racer is "me")

    // The track's map: one byte of coverage per pixel, rows from the top.
    // mapSerial changes when the picture does.
    unsigned mapSerial;
    int mapWidth, mapHeight;
    int mapMirrored;        // mirrored tracks: show it turned over left to right (the racers' places already are)
    const unsigned char *map;
    unsigned char mapColour[3];

    // The Adventure's collectables as the save has them.
    struct {
        int balloons;       // of 47
        int keys;           // of 4
        int wizpigAmulet;   // pieces, of 4
        int ttAmulet;       // pieces, of 4
        int trophies;       // worlds with a trophy of any colour, of 5
        int goldTrophies;
        int bosses;         // boss races won, of 10 (each of the four twice, Wizpig twice)
    } collect;
} BottomSnapshot;

// Game side (3ds/bottom_game.inc). The snapshot is filled in on the game
// thread once per frame while a level with racers is on screen.
const BottomSnapshot *pc_bottom_snapshot(void);

// Screen side (bottomscreen.c), called by touchmenu.c, which owns the frame
// buffer: 320 x 240, three bytes a pixel, columns first, bottom row first.
void bottomscreen_init(const char *dir);
// Once per controller poll while the settings menu is closed and the light is
// on. `redraw` asks for everything below the header again (the settings menu
// or the pause notice was over it). Returns 1 when it changed the frame
// buffer, 0 when the picture stands, -1 when this console is in a session's
// lobby or menus and touchmenu.c's own notice belongs there.
int bottomscreen_tick(unsigned char *frameBuffer, int redraw);
// The part of the frame buffer the last tick wrote, in bytes.
void bottomscreen_last_change(unsigned *offset, unsigned *size);
// A line under the banana bank on the menu page ("New character: ...").
void bottomscreen_set_note(const char *note);

#endif
