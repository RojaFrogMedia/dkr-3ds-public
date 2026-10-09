// 3DS entry point and frame pacing.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "diag.h"
#include "touchmenu.h"
#include "netplay.h"
#include "characters.h"
#include "gfx.h"

#define DKR_DIR "sdmc:/3ds/DKR"

void thread3_main(void *unused);
void autotest_init(const char *dir);
void autotest_frame(const char *dir, u64 workTicks);
int autotest_take_home(void);
int autotest_pause_shot(const char *dir);
void pc_log_sync(void);

void pc_host_thread_init(void);
void pc_gfx_set_local_view(int player);
void pc_gfx_task_run(void *dlBegin, void *dlEnd);

// ---------------------------------------------------------------------------
// The render thread. The game builds each frame's display list in buffers
// it alternates between frames, because on the N64 the RCP is still drawing
// the previous list while the CPU writes the next. The same split works
// here: the list is interpreted and drawn on another core while the game
// runs on. A new list waits for the one in flight.
// ---------------------------------------------------------------------------
static struct { void *begin, *end; } sRenderJob;
static volatile int sRenderBusy;
static LightEvent sRenderJobEvent, sRenderIdleEvent;
static LightLock sRenderLock;
static Thread sRenderThread;
extern volatile u64 gRenderCostTicks;   // the last frame's interpretation and submission (main.c)
static u64 sRenderWaitTicks;            // how long the game waited for it this frame

void pc_render_lock(void) {
    LightLock_Lock(&sRenderLock);
}

void pc_render_unlock(void) {
    LightLock_Unlock(&sRenderLock);
}

static void render_main(void *arg) {
    (void) arg;
    diag_thread_init();
    for (;;) {
        gDiagRenderPhase = DIAG_RENDER_IDLE;
        LightEvent_Wait(&sRenderJobEvent);
        pc_gfx_task_run(sRenderJob.begin, sRenderJob.end);
        gDiagRenderFrames++;
        sRenderBusy = 0;
        LightEvent_Signal(&sRenderIdleEvent);
    }
}

void pc_render_post(void *dlBegin, void *dlEnd) {
    if (sRenderThread == NULL) {
        pc_gfx_task_run(dlBegin, dlEnd);
        return;
    }
    if (sRenderBusy) {
        u64 t0 = svcGetSystemTick();

        gDiagMainPhase = DIAG_MAIN_WAIT_RENDER;
        while (sRenderBusy) {
            LightEvent_Wait(&sRenderIdleEvent);
        }
        gDiagMainPhase = DIAG_MAIN_LOGIC;
        sRenderWaitTicks += svcGetSystemTick() - t0;
    }
    sRenderJob.begin = dlBegin;
    sRenderJob.end = dlEnd;
    sRenderBusy = 1;
    LightEvent_Signal(&sRenderJobEvent);
}

// The same for the moments the program must not hang in (the HOME button,
// sleep, leaving): gives up after a second and says so. FALSE: gave up.
static int render_wait_idle_bounded(void) {
    int tries;

    for (tries = 0; sRenderThread != NULL && sRenderBusy && tries < 20; tries++) {
        LightEvent_WaitTimeout(&sRenderIdleEvent, 50000000ll);
    }
    if (sRenderThread != NULL && sRenderBusy) {
        diag_trail("the render thread did not finish its frame");
        return 0;
    }
    return 1;
}

// Returns once the render thread has finished the frame it was given.
void pc_render_wait_idle(void) {
    if (sRenderThread != NULL && sRenderBusy) {
        int phase = gDiagMainPhase;

        gDiagMainPhase = DIAG_MAIN_WAIT_RENDER;
        while (sRenderBusy) {
            LightEvent_Wait(&sRenderIdleEvent);
        }
        // The frame that posts next finds the event already consumed and
        // the thread idle, which is what it checks first.
        gDiagMainPhase = phase;
    }
}

static void render_thread_start(bool isNew) {
    FILE *f = fopen(DKR_DIR "/NO_RENDER_THREAD.TXT", "r");

    LightLock_Init(&sRenderLock);
    LightEvent_Init(&sRenderJobEvent, RESET_ONESHOT);
    LightEvent_Init(&sRenderIdleEvent, RESET_ONESHOT);
    if (f != NULL) {
        fclose(f);
        printf("RENDER: on the main thread (NO_RENDER_THREAD.TXT)\n");
        return;
    }
    // A New 3DS has a third core to itself; an Old 3DS shares the system's
    // second core with the audio mixer.
    APT_SetAppCpuTimeLimit(80);
    sRenderThread = threadCreate(render_main, NULL, 64 * 1024, 0x19, isNew ? 2 : 1, false);
    if (sRenderThread == NULL && isNew) {
        sRenderThread = threadCreate(render_main, NULL, 64 * 1024, 0x19, 1, false);
    }
    printf("RENDER: %s\n", sRenderThread != NULL ? "on its own thread" : "on the main thread (no thread)");
}
void pc_eeprom_flush(void);
void pc_eeprom_save_now(void);
void pc_audio_wait_idle(void);
int pc_audio_wait_idle_bounded(void);
void pc_audio_shutdown(void);

// ---------------------------------------------------------------------------
// The HOME menu, sleep and leaving.
//
// While the program is suspended the GPU belongs to the system. The render
// thread must not be in the middle of a frame then: it would wait for a GPU
// that never answers, and closing the game from the HOME menu with it in
// that state crashed the console. So before the system takes over (the hook
// runs on the game thread, inside aptMainLoop) the frame in flight is
// finished, and nothing new is posted until the game thread runs again.
// ---------------------------------------------------------------------------
static aptHookCookie sAptCookie;
static int sHomePaused;     // inside home_pause

static void apt_hook(APT_HookType type, void *param) {
    (void) param;
    switch (type) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
        case APTHOOK_ONEXIT:
            gDiagPaused = 1;
            diag_trail(type == APTHOOK_ONSUSPEND ? "HOME menu: handing over" :
                       type == APTHOOK_ONSLEEP ? "sleep: going to sleep" : "leaving: told to close");
            // APT hooks run as part of the OS handoff. Do not wait for worker
            // threads here: the OS may already have suspended their core, and
            // blocking inside this callback can hang the HOME transition. The
            // explicit pause path idles both workers before HOME is enabled;
            // exit cleanup waits again from normal game-thread context.
            diag_trail(type == APTHOOK_ONSUSPEND ? "HOME menu: open" :
                       type == APTHOOK_ONSLEEP ? "sleep: asleep" : "leaving: threads idle");
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            diag_trail(sHomePaused ? "paused (back from the HOME menu)" : "running");
            gDiagPaused = sHomePaused;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// The HOME button pauses first.
//
// The HOME menu is not allowed to open while the game runs
// (aptSetHomeAllowed), so a press of HOME reaches the game as "a press was
// refused". The game then stops where it is, with the render and audio
// threads idle and the touch screen saying PAUSED, and only now allows the
// HOME menu: a second press opens it, from a program that is standing still,
// and any other button goes back to the game. (Going straight to the HOME
// menu out of a running frame crashed the console.) Coming back from the
// HOME menu lands in the pause again.
// ---------------------------------------------------------------------------
static u64 sLastTick;

static void home_pause(void) {
    int renderIdle, audioIdle;

    sHomePaused = 1;
    gDiagPaused = 1;
    diag_trail("paused: HOME pressed");
    renderIdle = render_wait_idle_bounded();
    audioIdle = pc_audio_wait_idle_bounded();
    if (!renderIdle || !audioIdle) {
        // The system must not take the GPU or audio worker while either is
        // still using it. Stay in the game and let the next HOME press retry.
        sHomePaused = 0;
        gDiagPaused = 0;
        diag_trail("HOME pause cancelled: worker still busy");
        sLastTick = 0;
        return;
    }
    // Everything worth keeping is written now, while nothing is in a hurry:
    // closing from the HOME menu then has nothing left to save.
    pc_eeprom_save_now();
    modchar_save();
    touchmenu_show_paused(1);
    diag_trail("paused");
    // The HOME menu stays shut here too. Every run of a console that froze
    // ended with the trail at "HOME menu: open": the game had handed over to
    // the HOME menu and never ran again. So the game no longer hands over at
    // all: a second press of HOME closes the game itself, the ordinary way
    // (platform_shutdown), which returns to the Homebrew Launcher or the
    // HOME menu with nothing of the game left running.
    if (!autotest_pause_shot(DKR_DIR)) {
        for (;;) {
            if (!aptMainLoop()) {
                exit(0);
            }
            if (aptCheckHomePressRejected()) {
                diag_trail("leaving: HOME pressed again");
                exit(0);
            }
            hidScanInput();
            if (hidKeysDown() != 0) {
                break;
            }
            gspWaitForVBlank();
        }
    }
    touchmenu_show_paused(0);
    sHomePaused = 0;
    gDiagPaused = 0;
    diag_trail("running");
    sLastTick = 0;      // the pause is not a long frame
}

// Every way out goes through exit(), and so through here: the services are
// closed in order, with the other threads idle.
static void platform_shutdown(void) {
    gDiagPaused = 1;
    diag_trail("leaving: saving");
    netplay_end();              // the other consoles hear that this one left
    netplay_lobby_leave();
    pc_eeprom_save_now();
    modchar_save();
    diag_trail("leaving: stopping the threads");
    render_wait_idle_bounded();
    pc_audio_wait_idle_bounded();
    diag_trail("leaving: closing sound");
    pc_audio_shutdown();
    diag_trail("leaving: closing graphics");
    gfx_shutdown();
    diag_trail("ended");
    pc_log_sync();
}

// The game's own heap and the textures come out of the application heap;
// the linear heap only holds the vertex buffer, audio buffers and textures.
u32 __stacksize__ = 256 * 1024;

// ---------------------------------------------------------------------------
// Frame pacing: stands in for the VI retrace interrupt. The return value is
// the game's logic update rate, the number of 60 Hz periods this frame
// covers, which the game uses as its timestep.
//
// Like retail fb_update(), the rate is committed rather than measured each
// frame: a timestep that flickers 1,2,1,1,2 makes everything shake. The
// commitment starts at one period (60 fps); it rises to two when frames keep
// overrunning one period and returns when they have fitted with room to
// spare for a while.
//
// A period is one refresh of the screen, and the wait is for the screen's own
// vertical blank (gfx_vblank_wait), not for a timer: a timer set to the
// refresh rate still drifts against it, and where the two cross, frames are
// shown twice and skipped by turns. Counting blanks also makes the timestep
// exact: it is the number of refreshes the frame was on screen for.
// PERIOD_TICKS (59.83 Hz) is only the yardstick a frame's cost is held to.
// ---------------------------------------------------------------------------
#define PERIOD_TICKS 4481136ull
#define MAX_UPDATE_RATE 6

static u64 sWorkStart;
static u32 sLastVblank;
static int sCommitted = 1;
static int sOverruns, sFits;

// For the log's once-a-second report (autotest.c): frames that were on screen
// for longer than the committed rate, and the refreshes those took extra.
u32 gProfLateFrames, gProfLateRefreshes;

s32 pc_retrace_wait(void) {
    u64 now = svcGetSystemTick();
    u64 work, own, waited;
    u32 blank, target;
    s32 periods;

    if (!aptMainLoop()) {
        exit(0);
    }
    if (aptCheckHomePressRejected() || autotest_take_home()) {
        home_pause();
        now = svcGetSystemTick();
    }
    if (sLastTick == 0) {
        sLastTick = now;
        sWorkStart = now;
        sLastVblank = gfx_vblank_count();
        return netplay_fixed_rate() != 0 ? netplay_fixed_rate() : sCommitted;
    }
    // What a frame costs is the larger of the game's own time and the render
    // thread's: the two run side by side. Time spent waiting for the render
    // thread is not the game's.
    waited = sRenderWaitTicks;
    own = now - sWorkStart - waited;
    work = own;
    sRenderWaitTicks = 0;
    if (gRenderCostTicks > work) {
        work = gRenderCostTicks;
    }
    pc_eeprom_flush();
    autotest_frame(DKR_DIR, work);

    if (sCommitted == 1) {
        if (work > PERIOD_TICKS) {
            if (++sOverruns >= 6) {         // six of the recent frames did not fit
                sCommitted = 2;
                sOverruns = sFits = 0;
            }
        } else if (sOverruns > 0 && ++sFits >= 30) {
            sOverruns = sFits = 0;
        }
    } else if (work < PERIOD_TICKS * 85 / 100) {
        if (++sFits >= 45) {                // a second and a half inside one period
            sCommitted = 1;
            sOverruns = sFits = 0;
        }
    } else {
        sFits = 0;
    }

    // The menu can hold the rate at 60 or 30 instead.
    if (touchmenu_frame_rate() == MENU_RATE_60) {
        sCommitted = 1;
    } else if (touchmenu_frame_rate() == MENU_RATE_30) {
        sCommitted = 2;
    }

    // A netplay session runs every console at the one rate its host chose:
    // the timestep is part of what has to be the same everywhere.
    if (netplay_fixed_rate() != 0) {
        sCommitted = netplay_fixed_rate();
    }

    // Wait for the blank that ends this frame's refreshes. A frame that has
    // already passed it goes straight on, a little late, and catches up or,
    // once a whole refresh behind, counts as one period more.
    target = sLastVblank + (u32) sCommitted;
    blank = gfx_vblank_count();
    while ((s32) (blank - target) < 0) {
        u32 next;

        gDiagMainPhase = DIAG_MAIN_SLEEP;
        next = gfx_vblank_wait(GFX_VBLANK_GAME, blank);
        if (next == blank) {
            // No blank in 50 ms (suspended, or the callback was taken):
            // carry on as if it had come, and count from wherever it is now.
            sLastVblank = gfx_vblank_count() - (u32) sCommitted;
            blank = sLastVblank + (u32) sCommitted;
            break;
        }
        blank = next;
    }
    gDiagMainPhase = DIAG_MAIN_LOGIC;
    gDiagMainFrames++;
    periods = (s32) (blank - sLastVblank);
    sLastVblank = blank;
    sLastTick = svcGetSystemTick();
    sWorkStart = sLastTick;
    if (periods > sCommitted) {
        gProfLateFrames++;
        gProfLateRefreshes += (u32) (periods - sCommitted);
        // Why, for the first few of each second (autotest.c clears the
        // count): the game's own time, how long it then waited for the
        // render thread, and what that thread's last frame cost.
        if (gProfLateFrames <= 3) {
            printf("late: %ld refreshes for one frame: game %.1f ms, waited for the render thread %.1f ms, its frame %.1f ms "
                   "(of that waiting for the GPU %.1f ms)\n", (long) periods, (double) own * 1000.0 / SYSCLOCK_ARM11,
                   (double) waited * 1000.0 / SYSCLOCK_ARM11, (double) gRenderCostTicks * 1000.0 / SYSCLOCK_ARM11,
                   (double) gfx_gpu_wait_ticks() * 1000.0 / SYSCLOCK_ARM11);
        }
    }
    if (netplay_fixed_rate() != 0) {
        // A slow frame slows the game down rather than lengthening the step.
        return sCommitted;
    }
    if (periods < sCommitted) {
        periods = sCommitted;
    }
    if (periods > MAX_UPDATE_RATE) {
        periods = MAX_UPDATE_RATE;  // a long stall: do not try to catch up
    }
    return periods;
}

int main(int argc, char **argv) {
    bool isNew = false;

    (void) argc; (void) argv;
    mkdir(DKR_DIR, 0777);
    diag_trail_start(DKR_DIR);
    autotest_init(DKR_DIR);
    APT_CheckNew3DS(&isNew);
    osSetSpeedupEnable(true);               // 804 MHz and the L2 cache on a New 3DS
    printf("=== DKR 3DS (native) on %s 3DS ===\n", isNew ? "a New" : "an Old");

    diag_init();
    pc_host_thread_init();
    gfx_window_init(320, 240, 1);
    touchmenu_init(DKR_DIR);
    render_thread_start(isNew);
    // After citro3d's and the menu's own hooks, so this one runs first.
    aptHook(&sAptCookie, apt_hook, NULL);
    aptSetHomeAllowed(false);   // HOME pauses first: home_pause
    atexit(platform_shutdown);
    netplay_init(DKR_DIR);
    modchar_init(DKR_DIR);
    thread3_main(0);
    return 0;
}
