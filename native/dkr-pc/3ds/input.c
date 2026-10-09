// The 3DS pad as an N64 controller. A = accelerate, B = brake, R = hop and
// slide, L or ZL = Z (use item), START = START; the D-pad is the N64 D-pad
// and X/Y/ZR and the C-Stick are the C buttons. SELECT belongs to the touch
// screen menu (touchmenu.c), which also holds the stick settings. AUTOTEST.TXT
// next to the program scripts input for unattended runs (see autotest.c).
#include "input.h"
#include "touchmenu.h"

#include <3ds.h>
#include <stdio.h>

#define BTN_A 0x8000
#define BTN_B 0x4000
#define BTN_Z 0x2000
#define BTN_START 0x1000
#define BTN_UP 0x0800
#define BTN_DOWN 0x0400
#define BTN_LEFT 0x0200
#define BTN_RIGHT 0x0100
#define BTN_L 0x0020
#define BTN_R 0x0010
#define BTN_CUP 0x0008
#define BTN_CDOWN 0x0004
#define BTN_CLEFT 0x0002
#define BTN_CRIGHT 0x0001
#define BTN_TEST_FINISH 0x0040

#define STICK_MAX 80
#define KEYS_CSTICK (KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT)

extern u32 autotest_keys(void);
extern bool cpp_open(void);
extern u32 cpp_poll(void);
extern bool cpp_connected(void);

// The second pad and ZL/ZR: built into a New 3DS (ir:rst, which libctru
// merges into the key state once it is initialised), a Circle Pad Pro on an
// Old 3DS (cpp.c). Without either the game is fully playable; the C buttons
// are then on X and Y only.
static void second_pad_init(void) {
    bool isNew = false;

    APT_CheckNew3DS(&isNew);
    if (isNew) {
        irrstInit();
    } else {
        cpp_open();
    }
}

static const struct { u32 key; unsigned short button; } sBindings[] = {
    { KEY_A, BTN_A }, { KEY_B, BTN_B }, { KEY_R, BTN_R }, { KEY_L | KEY_ZL, BTN_Z }, { KEY_START, BTN_START },
    { KEY_DUP, BTN_UP }, { KEY_DDOWN, BTN_DOWN }, { KEY_DLEFT, BTN_LEFT }, { KEY_DRIGHT, BTN_RIGHT },
    { KEY_X | KEY_CSTICK_UP, BTN_CUP }, { KEY_Y | KEY_CSTICK_DOWN, BTN_CDOWN },
    { KEY_CSTICK_LEFT, BTN_CLEFT }, { KEY_ZR | KEY_CSTICK_RIGHT, BTN_CRIGHT },
};

static int scale_axis(int v) {
    int deadzone = touchmenu_stick_deadzone();

    if (v > -deadzone && v < deadzone) {
        return 0;
    }
    v = v * STICK_MAX / touchmenu_stick_range();
    return v > STICK_MAX ? STICK_MAX : (v < -STICK_MAX ? -STICK_MAX : v);
}

void input_host_read(unsigned short *button, signed char *stickX, signed char *stickY) {
    circlePosition pos;
    u32 held, script;
    unsigned short buttons = 0;
    unsigned i;
    int x, y;

    static int sInit = 0;
    static int sCppWasConnected = 0;

    if (!sInit) {
        sInit = 1;
        second_pad_init();
    }
    hidScanInput();
    held = hidKeysHeld() | cpp_poll();
    if (cpp_connected() != sCppWasConnected) {
        sCppWasConnected = cpp_connected();
        printf("INPUT: Circle Pad Pro %s\n", sCppWasConnected ? "connected" : "disconnected");
    }
    hidCircleRead(&pos);
    x = scale_axis(pos.dx);
    y = scale_axis(pos.dy);

    script = autotest_keys();
    held |= script;
    if (!touchmenu_cstick_enabled()) {
        held &= ~KEYS_CSTICK;
    }
    held = touchmenu_tick(held);
    if (script & KEY_CPAD_LEFT) x = -STICK_MAX;
    if (script & KEY_CPAD_RIGHT) x = STICK_MAX;
    if (script & KEY_CPAD_UP) y = STICK_MAX;
    if (script & KEY_CPAD_DOWN) y = -STICK_MAX;

    for (i = 0; i < sizeof(sBindings) / sizeof(sBindings[0]); i++) {
        if (held & sBindings[i].key) {
            buttons |= sBindings[i].button;
        }
    }
    // A test script's FINISH key: a bit no N64 button uses, which the race
    // logic reads as "this player has completed the race" (src/objects.c).
    if (script & BIT(12)) {
        buttons |= BTN_TEST_FINISH;
    }
    *button = buttons;
    *stickX = (signed char) x;
    *stickY = (signed char) y;
}
