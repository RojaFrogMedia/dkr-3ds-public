// The settings menu on the touch screen (touchmenu.c).
#ifndef DKR_3DS_TOUCHMENU_H
#define DKR_3DS_TOUCHMENU_H

// What the Stats page shows; autotest.c fills it in once a second.
typedef struct {
    unsigned fps;
    float workMs, workMaxMs;        // a frame's cost: the larger of game and render thread
    float displayListMs, presentMs, audioMs;
    float gpuMs;                    // the GPU's own time for a frame
    unsigned lateFrames;            // frames in the last second that stayed on screen too long
    unsigned vertices, batches;     // per frame
} MenuStats;

void touchmenu_init(const char *dir);

// Once per controller poll, with the pad as libctru reports it plus the
// scripted keys. Returns the keys the game may see: none of the menu's own
// while it is open.
unsigned touchmenu_tick(unsigned held);

void touchmenu_set_stats(const MenuStats *stats);

// The HOME button's pause (home_pause in platform.c): the touch screen says
// so while it lasts, and gets its usual picture back afterwards.
void touchmenu_show_paused(int paused);

// The settings, for the code they steer.
enum { MENU_RATE_AUTO, MENU_RATE_60, MENU_RATE_30 };
int touchmenu_frame_rate(void);          // MENU_RATE_*
int touchmenu_hard_mode(void);           // computer racers at an experienced player's pace (hardmode.c)
int touchmenu_save_file(void);           // 0: the player's own save, 1: the one with everything unlocked
int touchmenu_stick_deadzone(void);      // Circle Pad units inside which the stick reads zero
int touchmenu_stick_range(void);         // Circle Pad reading taken as full tilt
int touchmenu_cstick_enabled(void);      // the second pad works the C buttons
int touchmenu_volume_percent(void);

#endif
