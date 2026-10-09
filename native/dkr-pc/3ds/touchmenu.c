// The settings menu on the touch screen, after the ones in the Donkey Kong 64
// and F-Zero X ports: SELECT (or a tap) opens and closes it, tabs across the
// top, one setting per row.
//
//   D-pad up/down     choose a row (the tab row is above the first)
//   D-pad left/right  change the row's value, or the page on the tab row
//   touch             tabs, rows and the [-] [+] buttons
//
// While it is open the game does not see the D-pad or SELECT; everything
// else still plays. Values go to settings.ini in the game's folder a second
// after the last change. The menu is drawn straight into the bottom screen's
// framebuffer with libctru's 8x8 console font, and only when something on
// it changes, so it costs nothing while racing.
//
// While the menu is closed the screen below the header is bottomscreen.c's:
// the race display, the Adventure's collectables, or the logo and the banana
// bank. Only in a session's lobby and menus does this file draw there itself
// (draw_idle: which player the console is, the game's code).
//
// To add a setting: a variable, a line in sRows, a getter in menu.h for the
// code that uses it.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "touchmenu.h"
#include "bottom.h"
#include "characters.h"
#include "netplay.h"

extern const u8 default_font_bin[];         // libctru's console font: 256 glyphs of 8 rows
extern void pc_audio_set_volume(int percent);
extern bool cpp_connected(void);
extern u32 gAlEvtqDropped;

// ---- the screen: 320 x 240, 40 x 30 characters

#define SCREEN_W 320
#define SCREEN_H 240
#define RGB(r, g, b) (((u32) (r) << 16) | ((u32) (g) << 8) | (u32) (b))

#define COL_BACK    RGB(14, 22, 52)
#define COL_BAR     RGB(196, 40, 40)
#define COL_TAB     RGB(34, 48, 96)
#define COL_TAB_ON  RGB(250, 200, 40)
#define COL_ROW_ON  RGB(40, 70, 150)
#define COL_BUTTON  RGB(60, 90, 170)
#define COL_TEXT    RGB(255, 255, 255)
#define COL_DIM     RGB(150, 165, 200)
#define COL_DARK    RGB(20, 20, 30)
#define COL_VALUE   RGB(255, 230, 120)

static u8 *sFb;         // 240 x 320, three bytes a pixel, columns first, bottom row first

static void fill_rect(int x, int y, int w, int h, u32 colour) {
    int ix, iy;

    for (ix = x; ix < x + w; ix++) {
        u8 *column;

        if (ix < 0 || ix >= SCREEN_W) {
            continue;
        }
        column = sFb + ix * SCREEN_H * 3;
        for (iy = y; iy < y + h; iy++) {
            if (iy >= 0 && iy < SCREEN_H) {
                u8 *p = column + (SCREEN_H - 1 - iy) * 3;
                p[0] = (u8) colour;
                p[1] = (u8) (colour >> 8);
                p[2] = (u8) (colour >> 16);
            }
        }
    }
}

// Text at a pixel position; scale 2 doubles the glyphs.
static void draw_text(int x, int y, const char *text, u32 colour, int scale) {
    for (; *text != '\0'; text++, x += 8 * scale) {
        const u8 *glyph = default_font_bin + (u8) *text * 8;
        int gx, gy;

        for (gy = 0; gy < 8; gy++) {
            for (gx = 0; gx < 8; gx++) {
                if (glyph[gy] & (0x80 >> gx)) {
                    fill_rect(x + gx * scale, y + gy * scale, scale, scale, colour);
                }
            }
        }
    }
}

static void draw_text_centred(int y, const char *text, u32 colour, int scale) {
    draw_text((SCREEN_W - (int) strlen(text) * 8 * scale) / 2, y, text, colour, scale);
}

static void present(void) {
    GSPGPU_FlushDataCache(sFb, SCREEN_W * SCREEN_H * 3);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

// ---- the settings

enum { PAGE_GAME, PAGE_CONTROLS, PAGE_STATS, PAGE_ABOUT, PAGE_COUNT };
static const char *const sPageNames[PAGE_COUNT] = { "Game", "Controls", "Stats", "About" };

static int sFrameRate = MENU_RATE_AUTO;
static int sVolume = 100;
static int sScreenOff = 0;
static int sDeadzone = 12;
static int sRange = 140;
static int sCStick = 1;
static int sSaveFile = 0;
static int sHardMode = 0;

static const char *const sRateNames[] = { "Auto", "60 fps", "30 fps" };
static const char *const sScreenNames[] = { "Always on", "Off in play" };
static const char *const sCStickNames[] = { "Off", "C buttons" };
static const char *const sSaveNames[] = { "My save", "All unlocked" };
static const char *const sHardNames[] = { "Off", "On" };

static void apply_volume(int value) {
    pc_audio_set_volume(value);
}

typedef struct {
    int page;
    const char *key;            // in settings.ini
    const char *label;
    int *value;
    int lo, hi, step;
    const char *const *names;   // shown instead of the number
    const char *suffix;
    void (*apply)(int value);
    const char *help;           // shown under the rows while this one is chosen
} Row;

static const Row sRows[] = {
    { PAGE_GAME, "frame_rate", "Frame rate", &sFrameRate, 0, 2, 1, sRateNames, NULL, NULL,
      "Auto runs at 60 and drops to 30 while a scene is too heavy." },
    { PAGE_GAME, "volume", "Volume", &sVolume, 0, 100, 10, NULL, "%", apply_volume,
      "Everything the game plays. Music and effects have their own sliders in the game's options." },
    { PAGE_GAME, "screen_off", "Touch screen", &sScreenOff, 0, 1, 1, sScreenNames, NULL, NULL,
      "Off in play: the light goes out when this menu closes. Touch or SELECT brings it back." },
    { PAGE_GAME, "save_file", "Save file", &sSaveFile, 0, 1, 1, sSaveNames, NULL, NULL,
      "All unlocked: every track, character and trophy, kept in its own file. Takes effect the next time the game "
      "starts." },
    { PAGE_GAME, "hard_mode", "Hard mode", &sHardMode, 0, 1, 1, sHardNames, NULL, NULL,
      "Computer racers keep an experienced player's pace: between T.T.'s ghost and the record times. Not in "
      "multiplayer." },
    { PAGE_CONTROLS, "stick_deadzone", "Stick dead zone", &sDeadzone, 0, 40, 2, NULL, NULL, NULL,
      "How far the Circle Pad moves before the game notices." },
    { PAGE_CONTROLS, "stick_range", "Stick range", &sRange, 100, 160, 5, NULL, NULL, NULL,
      "The Circle Pad reading that counts as full tilt: lower steers harder." },
    { PAGE_CONTROLS, "cstick", "C-Stick", &sCStick, 0, 1, 1, sCStickNames, NULL, NULL,
      "The New 3DS C-Stick or a Circle Pad Pro's second pad." },
};
#define ROW_COUNT ((int) (sizeof(sRows) / sizeof(sRows[0])))

int touchmenu_frame_rate(void) { return sFrameRate; }
int touchmenu_save_file(void) { return sSaveFile; }
int touchmenu_hard_mode(void) { return sHardMode; }
int touchmenu_stick_deadzone(void) { return sDeadzone; }
int touchmenu_stick_range(void) { return sRange; }
int touchmenu_cstick_enabled(void) { return sCStick; }
int touchmenu_volume_percent(void) { return sVolume; }

// ---- settings.ini

static char sIniPath[160];
static int sSaveCountdown;      // polls until the file is written; 0 = nothing to save

static void settings_load(void) {
    FILE *f = fopen(sIniPath, "r");
    char line[96], key[48];
    int value, i;

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, " %47[^= ] = %d", key, &value) != 2) {
            continue;
        }
        for (i = 0; i < ROW_COUNT; i++) {
            if (strcmp(key, sRows[i].key) == 0 && value >= sRows[i].lo && value <= sRows[i].hi) {
                *sRows[i].value = value;
            }
        }
    }
    fclose(f);
}

static void settings_save(void) {
    FILE *f = fopen(sIniPath, "w");
    int i;

    if (f == NULL) {
        return;
    }
    fprintf(f, "# Diddy Kong Racing 3DS settings (the touch screen menu writes this file)\n");
    for (i = 0; i < ROW_COUNT; i++) {
        fprintf(f, "%s=%d\n", sRows[i].key, *sRows[i].value);
    }
    fclose(f);
}

// ---- the backlight of the touch screen

static bool sLcdReady;
static bool sLightOff;
static aptHookCookie sAptCookie;

static void backlight(bool on) {
    if (!sLcdReady || sLightOff == !on) {
        return;
    }
    if (on) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    } else {
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    }
    sLightOff = !on;
}

// The HOME menu and sleep get the light back; returning restores our state.
static void apt_hook(APT_HookType type, void *param) {
    (void) param;
    if (!sLcdReady || !sLightOff) {
        return;
    }
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONEXIT) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    } else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) {
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    }
}

static void backlight_restore_at_exit(void) {
    if (sLcdReady && sLightOff) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    }
}

// ---- layout (pixels)

#define TAB_Y 22
#define TAB_H 18
#define TAB_W (SCREEN_W / PAGE_COUNT)
#define ROW_Y0 50
#define ROW_STEP 26
#define ROW_H 22
#define LABEL_X 8
#define MINUS_X 160
#define VALUE_X 188
#define VALUE_W 96
#define PLUS_X 288
#define BUTTON_W 24
#define HELP_Y 178

static bool sOpen;
static bool sDirty = true;         // the whole screen needs drawing
static bool sHeaderDirty;           // only the frame rate in the header does
static int sPage;
static int sSel = -1;           // row within the page, -1 = the tab row
static MenuStats sStats;
static bool sIsNew;

static int page_rows(int page, int *indices) {
    int i, n = 0;

    for (i = 0; i < ROW_COUNT; i++) {
        if (sRows[i].page == page) {
            if (indices != NULL) {
                indices[n] = i;
            }
            n++;
        }
    }
    return n;
}

static void row_value_text(const Row *row, char *out, size_t size) {
    if (row->names != NULL) {
        snprintf(out, size, "%s", row->names[*row->value - row->lo]);
    } else {
        snprintf(out, size, "%d%s", *row->value, row->suffix != NULL ? row->suffix : "");
    }
}

// Text broken at spaces into lines of at most 38 characters.
static void draw_wrapped(int y, const char *text, u32 colour) {
    char line[40];

    while (*text != '\0') {
        size_t len = strlen(text), cut = len;

        if (len > 38) {
            for (cut = 38; cut > 0 && text[cut] != ' '; cut--) {
            }
            if (cut == 0) {
                cut = 38;
            }
        }
        memcpy(line, text, cut);
        line[cut] = '\0';
        draw_text(LABEL_X, y, line, colour, 1);
        y += 10;
        text += cut;
        while (*text == ' ') {
            text++;
        }
    }
}

// In a Local Play session the screen says which player this console is:
// 1 to 4, or 0 outside a session.
static int sPlayerShown;
static char sCodeShown[20];     // an online game's code while this console hosts it
static int sRatingShown;        // this console's rating as last drawn
static int sIdleShown;          // the player being warned for leaving the pad alone, 1-based
static const u32 sPlayerColours[NETPLAY_MAX_PLAYERS] = { RGB(240, 60, 60), RGB(70, 130, 255), RGB(250, 210, 50),
                                                         RGB(70, 210, 90) };

static void draw_header(const char *right) {
    char left[32] = "DIDDY KONG RACING";

    if (sPlayerShown != 0) {
        snprintf(left, sizeof(left), "MULTIPLAYER - PLAYER %d", sPlayerShown);
    }
    fill_rect(0, 0, SCREEN_W, 18, COL_BAR);
    draw_text(8, 5, left, COL_TEXT, 1);
    draw_text(SCREEN_W - 8 - (int) strlen(right) * 8, 5, right, COL_TEXT, 1);
}

static void draw_stats_page(void) {
    char line[48];
    int y = ROW_Y0;

    snprintf(line, sizeof(line), "Frame rate      %u fps (%u late)", sStats.fps, sStats.lateFrames);
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    snprintf(line, sizeof(line), "Frame cost      %.1f ms (worst %.1f)", sStats.workMs, sStats.workMaxMs);
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    snprintf(line, sizeof(line), "  drawing       %.1f ms", sStats.displayListMs);
    draw_text(LABEL_X, y, line, COL_DIM, 1); y += 14;
    snprintf(line, sizeof(line), "  to the GPU    %.1f ms (GPU %.1f ms)", sStats.presentMs, sStats.gpuMs);
    draw_text(LABEL_X, y, line, COL_DIM, 1); y += 14;
    snprintf(line, sizeof(line), "  music engine  %.1f ms", sStats.audioMs);
    draw_text(LABEL_X, y, line, COL_DIM, 1); y += 14;
    snprintf(line, sizeof(line), "Vertices        %u in %u batches", sStats.vertices, sStats.batches);
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    snprintf(line, sizeof(line), "Memory free     %lu KB", (unsigned long) (envGetHeapSize() / 1024));
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    snprintf(line, sizeof(line), "Console         %s 3DS", sIsNew ? "New" : "Old");
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    snprintf(line, sizeof(line), "Second pad      %s", sIsNew ? "C-Stick" : (cpp_connected() ? "Circle Pad Pro" : "none"));
    draw_text(LABEL_X, y, line, COL_TEXT, 1); y += 14;
    if (gAlEvtqDropped != 0) {
        snprintf(line, sizeof(line), "Sound events lost  %lu", (unsigned long) gAlEvtqDropped);
        draw_text(LABEL_X, y, line, COL_VALUE, 1);
    }
    draw_wrapped(HELP_Y + 20, "A frame cost above 16.7 ms cannot hold 60 fps.", COL_DIM);
}

static void draw_about_page(void) {
    static const char *const lines[] = {
        "A            accelerate",
        "B            brake / reverse",
        "R            hop and slide",
        "L or ZL      use item (Z)",
        "X, Y, ZR     C-Up, C-Down, C-Right",
        "C-Stick      C buttons",
        "Circle Pad   steer",
        "START        pause",
        "SELECT       this menu",
    };
    int i, y = ROW_Y0 - 4;

    for (i = 0; i < (int) (sizeof(lines) / sizeof(lines[0])); i++, y += 12) {
        draw_text(LABEL_X, y, lines[i], COL_TEXT, 1);
    }
    draw_wrapped(y + 8, "The decompiled game built for the 3DS. Saves, settings and log.txt are in "
                        "sdmc:/3ds/DKR/.", COL_DIM);
}

static void draw_menu(void) {
    int indices[ROW_COUNT];
    int count = page_rows(sPage, indices);
    char text[48];
    int i;

    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BACK);
    snprintf(text, sizeof(text), "%u fps", sStats.fps);
    draw_header(text);

    for (i = 0; i < PAGE_COUNT; i++) {
        bool on = i == sPage;
        int x = i * TAB_W;

        fill_rect(x + 1, TAB_Y, TAB_W - 2, TAB_H, on ? COL_TAB_ON : COL_TAB);
        if (on && sSel == -1) {
            fill_rect(x + 1, TAB_Y + TAB_H, TAB_W - 2, 2, COL_TEXT);
        }
        draw_text(x + (TAB_W - (int) strlen(sPageNames[i]) * 8) / 2, TAB_Y + 5, sPageNames[i], on ? COL_DARK : COL_TEXT, 1);
    }

    if (sPage == PAGE_STATS) {
        draw_stats_page();
    } else if (sPage == PAGE_ABOUT) {
        draw_about_page();
    }
    for (i = 0; i < count; i++) {
        const Row *row = &sRows[indices[i]];
        int y = ROW_Y0 + i * ROW_STEP;

        if (i == sSel) {
            fill_rect(0, y, SCREEN_W, ROW_H, COL_ROW_ON);
        }
        draw_text(LABEL_X, y + 7, row->label, COL_TEXT, 1);
        fill_rect(MINUS_X, y + 2, BUTTON_W, ROW_H - 4, COL_BUTTON);
        draw_text(MINUS_X + 8, y + 7, "-", COL_TEXT, 1);
        row_value_text(row, text, sizeof(text));
        draw_text(VALUE_X + (VALUE_W - (int) strlen(text) * 8) / 2, y + 7, text, COL_VALUE, 1);
        fill_rect(PLUS_X, y + 2, BUTTON_W, ROW_H - 4, COL_BUTTON);
        draw_text(PLUS_X + 8, y + 7, "+", COL_TEXT, 1);
    }
    if (sSel >= 0 && sSel < count && sRows[indices[sSel]].help != NULL) {
        draw_wrapped(HELP_Y, sRows[indices[sSel]].help, COL_DIM);
    }
    draw_text_centred(SCREEN_H - 12, "D-pad or touch   SELECT: back to game", COL_DIM, 1);
    present();
}

static int sBananasShown = -1;      // the banana bank as last seen
static bool sHudOwns;               // bottomscreen.c's picture is below the header

// The touch screen while the menu is closed, in a session's lobby and menus
// (bottomscreen.c has it otherwise).
static void draw_idle(void) {
    char text[48];

    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BACK);
    snprintf(text, sizeof(text), "%u fps", sStats.fps);
    draw_header(text);
    if (sPlayerShown != 0) {
        snprintf(text, sizeof(text), "PLAYER %d", sPlayerShown);
        draw_text_centred(58, "MULTIPLAYER", COL_TEXT, 2);
        fill_rect(40, 86, SCREEN_W - 80, 48, sPlayerColours[sPlayerShown - 1]);
        draw_text_centred(98, text, COL_DARK, 3);
        // The player's points (netplay.c): the profile's online, the
        // session's own over local wireless.
        snprintf(text, sizeof(text), "of %d     %d points", netplay_players(), netplay_rating(sPlayerShown - 1));
        draw_text_centred(142, text, COL_DIM, 1);
        if (sIdleShown != 0) {
            // Thirty seconds without input in a race; thirty more and the
            // player is out (pc_session_watch_idle in src/objects.c).
            fill_rect(0, 160, SCREEN_W, 44, COL_BAR);
            if (sIdleShown == sPlayerShown) {
                draw_text_centred(166, "YOU ARE INACTIVE", COL_TEXT, 2);
                draw_text_centred(188, "Move, or you will be disconnected", COL_TEXT, 1);
            } else {
                snprintf(text, sizeof(text), "PLAYER %d IS INACTIVE", sIdleShown);
                draw_text_centred(166, text, COL_TEXT, 2);
                draw_text_centred(188, "and will be disconnected", COL_TEXT, 1);
            }
        } else {
            draw_text_centred(170, "Touch or press SELECT for settings", COL_TEXT, 1);
        }
    } else if (sCodeShown[0] != '\0') {
        // Hosting an online game: the code to read out to the others.
        draw_text_centred(58, "GAME CODE", COL_TEXT, 2);
        fill_rect(20, 86, SCREEN_W - 40, 44, COL_TAB_ON);
        draw_text_centred(100, sCodeShown, COL_DARK, 2);
        draw_text_centred(142, "The other players enter it under", COL_DIM, 1);
        draw_text_centred(154, "MULTIPLAYER, ONLINE, JOIN A GAME", COL_DIM, 1);
        draw_text_centred(178, "Touch or press SELECT for settings", COL_TEXT, 1);
    }
    present();
}

// ---- input

#define MENU_KEYS (KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)
#define REPEAT_DELAY 24     // polls a direction is held before it repeats
#define REPEAT_EVERY 5
#define LIFT_POLLS 4        // the panel drops contact for a poll now and then

static void set_open(bool open) {
    sOpen = open;
    sDirty = true;
    backlight(open || !sScreenOff);
}

static void change_row(const Row *row, int direction) {
    int value = *row->value + direction * row->step;

    if (row->names != NULL) {       // a list of names goes round
        if (value > row->hi) value = row->lo;
        if (value < row->lo) value = row->hi;
    } else {
        if (value > row->hi) value = row->hi;
        if (value < row->lo) value = row->lo;
    }
    if (value != *row->value) {
        *row->value = value;
        if (row->apply != NULL) {
            row->apply(value);
        }
        sSaveCountdown = 60;
        sDirty = true;
    }
}

static void change_page(int direction) {
    sPage = (sPage + direction + PAGE_COUNT) % PAGE_COUNT;
    sDirty = true;
}

static void press(u32 key) {
    int indices[ROW_COUNT];
    int count = page_rows(sPage, indices);

    if (key == KEY_DUP) {
        sSel = sSel > -1 ? sSel - 1 : count - 1;
    } else if (key == KEY_DDOWN) {
        sSel = sSel < count - 1 ? sSel + 1 : -1;
    } else if (sSel == -1) {
        change_page(key == KEY_DRIGHT ? 1 : -1);
        return;
    } else {
        change_row(&sRows[indices[sSel]], key == KEY_DRIGHT ? 1 : -1);
        return;
    }
    sDirty = true;
}

static void tap(int x, int y) {
    int indices[ROW_COUNT];
    int count = page_rows(sPage, indices);
    int i;

    if (y >= TAB_Y - 4 && y < TAB_Y + TAB_H + 4) {
        sPage = x / TAB_W;
        sSel = -1;
        sDirty = true;
        return;
    }
    for (i = 0; i < count; i++) {
        int rowY = ROW_Y0 + i * ROW_STEP;

        if (y >= rowY && y < rowY + ROW_H) {
            sSel = i;
            sDirty = true;
            if (x >= MINUS_X - 6 && x < MINUS_X + BUTTON_W + 6) {
                change_row(&sRows[indices[i]], -1);
            } else if (x >= PLUS_X - 6) {
                change_row(&sRows[indices[i]], 1);
            }
            return;
        }
    }
}

extern bool autotest_touch(int *x, int *y);

unsigned touchmenu_tick(unsigned held) {
    static u32 sPrevHeld;
    static int sRepeat;
    static int sLifted = LIFT_POLLS;
    u32 down = held & ~sPrevHeld;
    touchPosition touch = { 0, 0 };
    int tx, ty;
    bool touching;

    if (sFb == NULL) {
        return held;
    }
    sPrevHeld = held;
    {
        int player = netplay_players() != 0 ? netplay_slot() + 1 : 0;

        if (player != sPlayerShown) {
            sPlayerShown = player;
            sDirty = true;
        }
        if (netplay_rating(player - 1) != sRatingShown) {
            sRatingShown = netplay_rating(player - 1);
            sDirty = true;
        }
        if (netplay_idle_warning() != sIdleShown) {
            sIdleShown = netplay_idle_warning();
            sDirty = true;
        }
        if (strcmp(sCodeShown, netplay_lobby_code()) != 0) {
            snprintf(sCodeShown, sizeof(sCodeShown), "%s", netplay_lobby_code());
            sDirty = true;
        }
    }

    touching = (hidKeysHeld() & KEY_TOUCH) != 0;
    if (touching) {
        hidTouchRead(&touch);
    }
    if (autotest_touch(&tx, &ty)) {
        touching = true;
        touch.px = (u16) tx;
        touch.py = (u16) ty;
    }

    if (down & KEY_SELECT) {
        set_open(!sOpen);
    }
    if (touching) {
        if (sLifted >= LIFT_POLLS) {        // a new touch
            if (!sOpen) {
                set_open(true);
            } else {
                tap(touch.px, touch.py);
            }
        }
        sLifted = 0;
    } else if (sLifted < LIFT_POLLS) {
        sLifted++;
    }

    if (sOpen) {
        u32 direction = held & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);

        if (down & direction) {
            press(down & direction & (u32) -(s32) (down & direction));      // the lowest of them
            sRepeat = 0;
        } else if (direction != 0 && (direction & (direction - 1)) == 0) {
            if (++sRepeat >= REPEAT_DELAY && (sRepeat - REPEAT_DELAY) % REPEAT_EVERY == 0 &&
                (direction & (KEY_DLEFT | KEY_DRIGHT)) && sSel >= 0) {
                press(direction);
            }
        } else {
            sRepeat = 0;
        }
    }

    if (sSaveCountdown > 0 && --sSaveCountdown == 0) {
        settings_save();
    }
    if (sBananasShown != modchar_bananas()) {
        const char *news = modchar_take_news();

        sBananasShown = modchar_bananas();
        if (news != NULL) {
            char note[40];

            snprintf(note, sizeof(note), "New character: %s", news);
            bottomscreen_set_note(note);
        }
    }
    if (sHeaderDirty && !sDirty) {
        char text[16];

        snprintf(text, sizeof(text), "%u fps", sStats.fps);
        draw_header(text);
        present();
    }
    sHeaderDirty = false;
    if (sOpen) {
        sHudOwns = false;
        if (sDirty) {
            sDirty = false;
            draw_menu();
        }
    } else if (!sLightOff) {
        // With the light out there is nobody to read it: what is due stays
        // due (sDirty) until the light is back.
        bool redraw = sDirty || !sHudOwns;
        int drew = bottomscreen_tick(sFb, redraw);

        if (drew < 0) {
            // A session's lobby or menus: this file's own notice.
            if (sHudOwns || sDirty) {
                sHudOwns = false;
                sDirty = false;
                draw_idle();
            }
        } else {
            sHudOwns = true;
            sDirty = false;
            if (redraw) {
                char text[16];

                snprintf(text, sizeof(text), "%u fps", sStats.fps);
                draw_header(text);
                present();
            } else if (drew > 0) {
                // Only what changed: flushing the whole screen forty times
                // a second in a race would be wasted on the game's thread.
                unsigned at, size;

                bottomscreen_last_change(&at, &size);
                if (size != 0) {
                    GSPGPU_FlushDataCache(sFb + at, size);
                }
            }
        }
    }
    return sOpen ? (held & ~MENU_KEYS) : (held & ~KEY_SELECT);
}

void touchmenu_show_paused(int paused) {
    if (!paused) {
        backlight(sOpen || !sScreenOff);
        sDirty = true;
        return;
    }
    backlight(true);    // also when the settings keep this screen dark
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BACK);
    draw_header("");
    draw_text_centred(62, "PAUSED", COL_TAB_ON, 3);
    fill_rect(0, 112, SCREEN_W, 44, COL_BAR);
    draw_text_centred(118, "Press HOME again", COL_TEXT, 2);
    draw_text_centred(140, "to close the game", COL_TEXT, 1);
    draw_text_centred(178, "Any other button: back to the game", COL_TEXT, 1);
    present();
}

void touchmenu_set_stats(const MenuStats *stats) {
    sStats = *stats;
    // The header carries the frame rate on every page, and only the Stats
    // page shows more of them; with the light off there is nobody to read it.
    if (sOpen && sPage == PAGE_STATS) {
        sDirty = true;
    } else if (sOpen || !sLightOff) {
        sHeaderDirty = true;
    }
}

void touchmenu_init(const char *dir) {
    int i;

    snprintf(sIniPath, sizeof(sIniPath), "%s/settings.ini", dir);
    settings_load();
    for (i = 0; i < ROW_COUNT; i++) {
        if (sRows[i].apply != NULL) {
            sRows[i].apply(*sRows[i].value);
        }
    }
    APT_CheckNew3DS(&sIsNew);

    // One buffer, drawn into in place: the menu changes a few times a
    // second at most.
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    sFb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    sLcdReady = R_SUCCEEDED(gspLcdInit());
    if (sLcdReady) {
        aptHook(&sAptCookie, apt_hook, NULL);
        atexit(backlight_restore_at_exit);
    }
    bottomscreen_init(dir);
    set_open(false);
}
