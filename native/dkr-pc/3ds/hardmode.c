// Hard mode: computer racers that keep an experienced player's pace.
//
// A switch on the touch screen menu (Game, "Hard mode"). With it on, the
// computer racers of an ordinary race are held to a pace taken from real
// times instead of the game's own easy one:
//
//   ttSeconds      T.T.'s ghost for the track, three laps: the time the
//                  game's makers set for good players. Read out of the
//                  game's own ghost data (and the same figures as in the
//                  T.T. guides on the internet).
//   recordSeconds  the fastest three laps on speedrun.com's level
//                  leaderboards (www.speedrun.com/dkr/levels, read
//                  2026-10-04). These use boosts and shortcuts no computer
//                  racer can.
//
// The leading computer racer aims at the middle of the two, which is about
// where experienced players are without tricks (Ancient Lake: T.T. 61.3 s,
// record 29.8 s, middle 45.5 s; the Guinness-listed time trial there is
// 47.4 s). Each further computer racer aims HARD_SPREAD slower than the one
// before, so they still arrive one after another.
//
// How it is held to it: the game sets a computer racer's speed in "bananas"
// (each is 2.5% of top speed, as for a player, but up to 20 of them), from a
// table of the level. Every frame hardmode_bananas works out how many the
// racer needs: more for every second it is behind its schedule, fewer when
// it is ahead; handle_racer_top_speed in src/racer.c takes the larger of that
// and the game's own figure. The game's limit of 20 stays: adding speed
// beyond it was tried and the racers missed their corners. To change the
// difficulty change HARD_* below or the table.
//
// What it does in practice (emulator, Ancient Lake, the player left standing):
// without it the computer racers took 80 to 84 s, with it 60 to 64 s, which
// is T.T.'s ghost's time. They were at the game's limit of 20 bananas the
// whole way and still behind the 45.5 s schedule: the schedule is the
// experts', the limit is the game's, and the limit wins.
//
// Not in a session between consoles: the switch is each console's own, and
// the consoles must run the same race.
#include <stdio.h>

#include "netplay.h"
#include "touchmenu.h"

#define HARD_BASE 6.0f          // bananas when exactly on schedule
#define HARD_PER_SECOND 2.5f    // more for every second behind, fewer for every second ahead
#define HARD_MOST 20.0f         // the game's own limit for computer racers
#define HARD_SPREAD 0.025f      // each further computer racer's schedule is this much slower

typedef struct {
    int level;              // ASSET_LEVEL_* (include/asset_enums.h)
    const char *name;
    float ttSeconds;
    float recordSeconds;
} HardTrack;

static const HardTrack sTracks[] = {
    { 5, "Ancient Lake", 61.28f, 29.81f },
    { 3, "Fossil Canyon", 90.18f, 51.93f },
    { 29, "Jungle Falls", 62.50f, 32.14f },
    { 7, "Hot Top Volcano", 90.72f, 42.06f },
    { 8, "Whale Bay", 73.12f, 42.14f },
    { 4, "Pirate Lagoon", 88.57f, 43.66f },
    { 10, "Crescent Island", 93.17f, 60.38f },
    { 30, "Treasure Caves", 65.63f, 28.71f },
    { 13, "Everfrost Peak", 108.00f, 50.39f },
    { 6, "Walrus Cove", 126.28f, 72.41f },
    { 9, "Snowball Valley", 66.42f, 35.78f },
    { 28, "Frosty Village", 99.40f, 55.63f },
    { 19, "Boulder Canyon", 122.35f, 67.45f },
    { 18, "Greenwood Village", 100.62f, 62.29f },
    { 20, "Windmill Plains", 133.13f, 56.71f },
    { 31, "Haunted Woods", 69.52f, 40.16f },
    { 17, "Spacedust Alley", 126.17f, 67.15f },
    { 32, "Darkmoon Caverns", 132.05f, 87.97f },
    { 33, "Star City", 121.45f, 58.80f },
    { 15, "Spaceport Alpha", 129.03f, 49.81f },
};

int hardmode_on(void) {
    return touchmenu_hard_mode() && netplay_players() == 0;
}

// The time the leading computer racer aims at on a track; 0 if the track is not in the table.
float hardmode_target_seconds(int level) {
    unsigned i;

    for (i = 0; i < sizeof(sTracks) / sizeof(sTracks[0]); i++) {
        if (sTracks[i].level == level) {
            return (sTracks[i].ttSeconds + sTracks[i].recordSeconds) * 0.5f;
        }
    }
    return 0.0f;
}

// The bananas' worth of speed a computer racer needs now; 0 with hard mode off.
//   place     0 for the first computer racer, 1 for the next...
//   progress  how much of the race it has done, 0 to 1
//   seconds   since the start
float hardmode_bananas(int level, int place, float progress, float seconds) {
    float target = hardmode_target_seconds(level);
    float behind, bananas;

    if (!hardmode_on() || target <= 0.0f) {
        return 0.0f;
    }
    target *= 1.0f + (HARD_SPREAD * place);
    behind = seconds - (target * progress);
    bananas = HARD_BASE + (HARD_PER_SECOND * behind);
    return bananas < 0.0f ? 0.0f : bananas > HARD_MOST ? HARD_MOST : bananas;
}

// For the log: a computer racer's time, and in hard mode its schedule.
void hardmode_note_finish(int level, int place, float seconds) {
    if (hardmode_on() && hardmode_target_seconds(level) > 0.0f) {
        printf("HARD: computer racer %d finished in %.2f s (schedule %.2f s)\n", place + 1, seconds,
               hardmode_target_seconds(level) * (1.0f + (HARD_SPREAD * place)));
    } else {
        printf("RACE: computer racer %d finished in %.2f s\n", place + 1, seconds);
    }
}
