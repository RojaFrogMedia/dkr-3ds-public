// Unattended runs: the log, scripted input, screenshots and a frame-time
// report, all in the game's folder on the SD card.
//
//   log.txt        stdout and stderr, written to the card by a thread of its own
//   AUTOTEST.TXT   one command per line, frames counted in rendered frames:
//                    <frame> <frames> KEY[+KEY]   hold keys (A B X Y L R ZL ZR START
//                                                 SELECT UP DOWN LEFT RIGHT STICK_UP ...;
//                                                 FINISH ends the race for this player)
//                    <frame> SHOT                 save shot_NN.bmp of the top screen
//                    <frame> TOUCH x,y            touch the bottom screen there for a few frames
//                    <frame> BSHOT                save bshot_NN.bmp of the bottom screen
//                    <frame> SOUND n              play sound id n (an added character's voice is read in first)
//                    <frame> BANKSOUND n          play recording n of the sound bank directly
//                    <frame> NOMUSIC              music volume to nothing (to record the effects alone)
//                    <frame> VOICECHECK           every added character's recordings, read and summed into the log
//                    <frame> HIDETEXT             the multiplayer screens draw no words (a picture of their sky)
//                    <frame> HOME                 as the HOME button: the pause, which a script
//                                                 leaves by itself after a screenshot of it
//                    <frame> COLUMNS n            character select: pretend the screen holds n columns
//                    <frame> SKIPLOGOS            from here a key ends the opening logos, as it does for
//                                                 a player (a script's keys leave them alone otherwise)
//                    <frame> EXIT                 leave
//                    <frame> JINGLE               play the locked-door jingle over the music
//                    <frame> JINGLE_END           bring the music back, as the door does
//                    <frame> AUDIO                log what the sequence players are doing
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <unistd.h>

#include "touchmenu.h"

#define MAX_CMDS 128
enum { CMD_KEYS, CMD_SHOT, CMD_EXIT, CMD_JINGLE, CMD_JINGLE_END, CMD_AUDIO, CMD_TOUCH, CMD_BSHOT, CMD_COLUMNS, CMD_HOME, CMD_NOMUSIC, CMD_SOUND, CMD_BANKSOUND, CMD_VOICECHECK, CMD_HIDETEXT, CMD_SKIPLOGOS };
#define TOUCH_FRAMES 3
typedef struct { int frame, frames, kind; u32 keys; } Cmd;

static Cmd sCmds[MAX_CMDS];
static int sCmdCount;
static int sFrame;
static int sShots, sBottomShots;
static u32 sKeys;
static int sTouching, sTouchX, sTouchY;

// Frame statistics over one-second windows.
static u64 sWindowStart;
static u32 sWindowFrames;
static u64 sWorkTicks, sWorkMax;
extern u64 gProfDlTicks, gProfPresentTicks, gProfAudioTicks, gProfGpuWaitTicks;
extern float gProfGpuMs, gProfGpuMaxMs;
extern u32 gProfLateFrames, gProfLateRefreshes;
extern u32 gProfTriVerts;
extern void pc_debug_jingle(s32 step);
extern int gLogosSkippable;     // src/menu.c
extern void pc_debug_audio_state(void);
extern void pc_debug_sound_counts(void);
extern u32 gProfTexCreates, gProfTexTexels, gProfSlowShades, gProfBatches, gProfDraws;

static u32 key_bit(const char *name) {
    static const struct { const char *n; u32 k; } tab[] = {
        { "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
        { "ZL", KEY_ZL }, { "ZR", KEY_ZR }, { "START", KEY_START }, { "SELECT", KEY_SELECT },
        { "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
        { "STICK_UP", KEY_CPAD_UP }, { "STICK_DOWN", KEY_CPAD_DOWN },
        { "STICK_LEFT", KEY_CPAD_LEFT }, { "STICK_RIGHT", KEY_CPAD_RIGHT },
        { "FINISH", BIT(12) },      // test only: this player's racer completes the race (input.c)
    };
    unsigned i;

    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcasecmp(tab[i].n, name) == 0) {
            return tab[i].k;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The log. Writing a line to the SD card takes a console tens of milliseconds
// (the emulator, none), and the once-a-second report used to be written by
// the game thread itself: one frame in sixty was late for it, which is the
// "58 fps" every console log showed whatever the frame cost. So printf only
// copies into a buffer here, and a thread of low priority writes the buffer
// to the card while the game thread waits for the screen.
// ---------------------------------------------------------------------------
#define LOG_RING 32768      // a power of two; about two minutes of the usual lines
static char sLogRing[LOG_RING];
static u32 sLogHead, sLogTail;      // bytes ever put in, bytes ever written out
static LightLock sLogRingLock, sLogFileLock;
static FILE *sLogFile;

// Writes out what is in the buffer. Returns when the card has it.
void pc_log_sync(void) {
    if (sLogFile == NULL) {
        return;
    }
    LightLock_Lock(&sLogFileLock);
    for (;;) {
        u32 head, at, n;

        LightLock_Lock(&sLogRingLock);
        head = sLogHead;
        LightLock_Unlock(&sLogRingLock);
        if (head == sLogTail) {
            break;
        }
        // Only this function moves the tail, one caller at a time, so the
        // bytes between tail and head stay put while they are written.
        at = sLogTail & (LOG_RING - 1);
        n = head - sLogTail;
        if (n > LOG_RING - at) {
            n = LOG_RING - at;
        }
        fwrite(sLogRing + at, 1, n, sLogFile);
        LightLock_Lock(&sLogRingLock);
        sLogTail += n;
        LightLock_Unlock(&sLogRingLock);
    }
    fflush(sLogFile);
    LightLock_Unlock(&sLogFileLock);
}

static ssize_t log_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    size_t done = 0;

    (void) r; (void) fd;
    while (done < len) {
        u32 at, n;

        LightLock_Lock(&sLogRingLock);
        at = sLogHead & (LOG_RING - 1);
        n = LOG_RING - (sLogHead - sLogTail);       // free
        if (n > len - done) {
            n = (u32) (len - done);
        }
        if (n > LOG_RING - at) {
            n = LOG_RING - at;
        }
        memcpy(sLogRing + at, ptr + done, n);
        sLogHead += n;
        LightLock_Unlock(&sLogRingLock);
        done += n;
        if (n == 0) {
            pc_log_sync();      // full: nothing is dropped, the writer waits instead
        }
    }
    return (ssize_t) len;
}

static void log_thread(void *arg) {
    (void) arg;
    for (;;) {
        svcSleepThread(500000000ll);
        pc_log_sync();
    }
}

static const devoptab_t sLogDevice = { .name = "dkrlog", .write_r = log_write };

static void log_open(const char *dir) {
    const devoptab_t *was[2];
    char path[160];

    snprintf(path, sizeof(path), "%s/log.txt", dir);
    sLogFile = fopen(path, "w");
    if (sLogFile == NULL) {
        return;
    }
    LightLock_Init(&sLogRingLock);
    LightLock_Init(&sLogFileLock);
    was[0] = devoptab_list[STD_OUT];
    was[1] = devoptab_list[STD_ERR];
    devoptab_list[STD_OUT] = &sLogDevice;
    devoptab_list[STD_ERR] = &sLogDevice;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    // Below the game thread (0x30) on its core: it runs while the game waits
    // for the screen, and the card itself is the system's work on another.
    if (threadCreate(log_thread, NULL, 16 * 1024, 0x3A, -2, true) == NULL) {
        // No thread: every line straight to the card, as it used to be.
        devoptab_list[STD_OUT] = was[0];
        devoptab_list[STD_ERR] = was[1];
        fclose(sLogFile);
        sLogFile = NULL;
        freopen(path, "w", stdout);
        setvbuf(stdout, NULL, _IOLBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
}

void autotest_init(const char *dir) {
    char path[160], line[128];
    FILE *f;

    log_open(dir);

    snprintf(path, sizeof(path), "%s/AUTOTEST.TXT", dir);
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL && sCmdCount < MAX_CMDS) {
        Cmd c = { 0, 0, CMD_KEYS, 0 };
        char a[32] = "", b[64] = "";
        char *tok;

        if (line[0] == '#' || sscanf(line, "%d %31s %63s", &c.frame, a, b) < 2) {
            continue;
        }
        if (strcasecmp(a, "SHOT") == 0) {
            c.kind = CMD_SHOT;
        } else if (strcasecmp(a, "BSHOT") == 0) {
            c.kind = CMD_BSHOT;
        } else if (strcasecmp(a, "HOME") == 0) {
            c.kind = CMD_HOME;
        } else if (strcasecmp(a, "NOMUSIC") == 0) {
            c.kind = CMD_NOMUSIC;
        } else if (strcasecmp(a, "VOICECHECK") == 0) {
            c.kind = CMD_VOICECHECK;
        } else if (strcasecmp(a, "HIDETEXT") == 0) {
            c.kind = CMD_HIDETEXT;
        } else if (strcasecmp(a, "SKIPLOGOS") == 0) {
            c.kind = CMD_SKIPLOGOS;
        } else if (strcasecmp(a, "SOUND") == 0 || strcasecmp(a, "BANKSOUND") == 0) {
            c.kind = strcasecmp(a, "SOUND") == 0 ? CMD_SOUND : CMD_BANKSOUND;
            c.keys = (u32) atoi(b);
        } else if (strcasecmp(a, "COLUMNS") == 0) {
            c.kind = CMD_COLUMNS;
            c.keys = (u32) atoi(b);
        } else if (strcasecmp(a, "TOUCH") == 0) {
            int x = 0, y = 0;

            sscanf(b, "%d,%d", &x, &y);
            c.kind = CMD_TOUCH;
            c.keys = (u32) x | ((u32) y << 16);
        } else if (strcasecmp(a, "EXIT") == 0) {
            c.kind = CMD_EXIT;
        } else if (strcasecmp(a, "JINGLE") == 0) {
            c.kind = CMD_JINGLE;
        } else if (strcasecmp(a, "JINGLE_END") == 0) {
            c.kind = CMD_JINGLE_END;
        } else if (strcasecmp(a, "AUDIO") == 0) {
            c.kind = CMD_AUDIO;
        } else {
            c.frames = atoi(a);
            for (tok = strtok(b, "+"); tok != NULL; tok = strtok(NULL, "+")) {
                c.keys |= key_bit(tok);
            }
        }
        sCmds[sCmdCount++] = c;
    }
    fclose(f);
    printf("AUTOTEST: %d commands\n", sCmdCount);
    // The scripts press their first keys while the logos may still be up.
    gLogosSkippable = 0;
}

u32 autotest_keys(void) {
    return sKeys;
}

// A scripted touch, for the menu.
bool autotest_touch(int *x, int *y) {
    *x = sTouchX;
    *y = sTouchY;
    return sTouching != 0;
}

static int sHomePressed;
static void save_shot(const char *dir, gfxScreen_t screen);

// A scripted HOME press, once (home_pause in platform.c).
int autotest_take_home(void) {
    int pressed = sHomePressed;

    sHomePressed = 0;
    return pressed;
}

// The pause under a script: a picture of the touch screen, and no waiting for a button.
int autotest_pause_shot(const char *dir) {
    if (sCmdCount == 0) {
        return 0;
    }
    save_shot(dir, GFX_BOTTOM);
    return 1;
}

static void save_shot(const char *dir, gfxScreen_t screen) {
    // A screen's framebuffer is 240 rows by 400 (top) or 320 (bottom) BGR8,
    // rotated: column = screen row from the bottom. Written as a bottom-up BMP.
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    const int width = screen == GFX_TOP ? 400 : 320;
    char path[160];
    u8 header[54] = { 'B', 'M' };
    u32 size = 54 + width * 240 * 3;
    FILE *f;
    int x, y;

    if (screen == GFX_TOP) {
        snprintf(path, sizeof(path), "%s/shot_%02d.bmp", dir, sShots++);
    } else {
        snprintf(path, sizeof(path), "%s/bshot_%02d.bmp", dir, sBottomShots++);
    }
    f = fopen(path, "wb");
    if (f == NULL || fb == NULL) {
        return;
    }
    memcpy(header + 2, &size, 4);
    header[10] = 54; header[14] = 40;
    header[18] = width & 0xFF; header[19] = width >> 8;
    header[22] = 240;
    header[26] = 1; header[28] = 24;
    fwrite(header, 1, 54, f);
    for (y = 0; y < 240; y++) {
        static u8 row[400 * 3];
        for (x = 0; x < width; x++) {
            const u8 *p = fb + (x * 240 + y) * 3;
            row[x * 3 + 0] = p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[2];
        }
        fwrite(row, 1, width * 3, f);
    }
    fclose(f);
    printf("AUTOTEST: shot %s\n", path);
}

// Called once per rendered frame with the time the frame's work took.
void autotest_frame(const char *dir, u64 workTicks) {
    u64 now = svcGetSystemTick();
    int i;

    sWorkTicks += workTicks;
    if (workTicks > sWorkMax) {
        sWorkMax = workTicks;
    }
    sWindowFrames++;
    if (sWindowStart == 0) {
        sWindowStart = now;
    } else if (now - sWindowStart >= SYSCLOCK_ARM11) {
        printf("stats: %lu fps, frame work avg %.1f ms max %.1f ms\n", (unsigned long) sWindowFrames,
               (double) sWorkTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames, (double) sWorkMax * 1000.0 / SYSCLOCK_ARM11);
        printf("prof: per frame: display list %.1f ms, present %.1f ms, audio %.1f ms, %lu vertices\n",
               (double) gProfDlTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames, (double) gProfPresentTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames,
               (double) gProfAudioTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames, (unsigned long) (gProfTriVerts / sWindowFrames));
        printf("prof: per second: %lu batches in %lu draw calls (%lu shaded the slow way), %lu textures made (%lu texels)\n",
               (unsigned long) gProfBatches, (unsigned long) gProfDraws, (unsigned long) gProfSlowShades, (unsigned long) gProfTexCreates,
               (unsigned long) gProfTexTexels);
        // The GPU's own time for a frame (it draws one frame while the next
        // is prepared, so this is beside the frame work, not part of it), how
        // long frames waited for it, and frames that stayed on screen longer
        // than the committed rate.
        printf("gpu: per frame: %.1f ms (longest %.1f), waited for %.1f ms; %lu frames late by %lu refreshes\n",
               (double) gProfGpuMs / sWindowFrames, (double) gProfGpuMaxMs,
               (double) gProfGpuWaitTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames, (unsigned long) gProfLateFrames,
               (unsigned long) gProfLateRefreshes);
        pc_debug_sound_counts();
        {
            MenuStats stats;

            stats.fps = sWindowFrames;
            stats.workMs = (float) ((double) sWorkTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames);
            stats.workMaxMs = (float) ((double) sWorkMax * 1000.0 / SYSCLOCK_ARM11);
            stats.displayListMs = (float) ((double) gProfDlTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames);
            stats.presentMs = (float) ((double) gProfPresentTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames);
            stats.audioMs = (float) ((double) gProfAudioTicks * 1000.0 / SYSCLOCK_ARM11 / sWindowFrames);
            stats.gpuMs = (float) ((double) gProfGpuMs / sWindowFrames);
            stats.lateFrames = gProfLateFrames;
            stats.vertices = gProfTriVerts / sWindowFrames;
            stats.batches = gProfBatches / sWindowFrames;
            touchmenu_set_stats(&stats);
        }
        gProfDlTicks = gProfPresentTicks = gProfAudioTicks = gProfGpuWaitTicks = 0;
        gProfGpuMs = gProfGpuMaxMs = 0.0f;
        gProfLateFrames = gProfLateRefreshes = 0;
        gProfTriVerts = 0;
        gProfBatches = gProfDraws = gProfSlowShades = gProfTexCreates = gProfTexTexels = 0;
        sWindowStart = now;
        sWindowFrames = 0;
        sWorkTicks = sWorkMax = 0;
    }

    sFrame++;
    sKeys = 0;
    sTouching = 0;
    for (i = 0; i < sCmdCount; i++) {
        const Cmd *c = &sCmds[i];
        if (c->kind == CMD_KEYS && sFrame >= c->frame && sFrame < c->frame + c->frames) {
            sKeys |= c->keys;
        } else if (c->kind == CMD_TOUCH && sFrame >= c->frame && sFrame < c->frame + TOUCH_FRAMES) {
            sTouching = 1;
            sTouchX = (int) (c->keys & 0xFFFF);
            sTouchY = (int) (c->keys >> 16);
        } else if (c->kind == CMD_SHOT && sFrame == c->frame) {
            save_shot(dir, GFX_TOP);
        } else if (c->kind == CMD_COLUMNS && sFrame == c->frame) {
            extern s32 gCharSelectVisibleColumns;       // src/menu.c

            gCharSelectVisibleColumns = (s32) c->keys;
        } else if (c->kind == CMD_BSHOT && sFrame == c->frame) {
            save_shot(dir, GFX_BOTTOM);
        } else if (c->kind == CMD_HOME && sFrame == c->frame) {
            sHomePressed = 1;
        } else if ((c->kind == CMD_SOUND || c->kind == CMD_BANKSOUND) && sFrame == c->frame) {
            extern void pc_debug_sound(int id, int direct);

            pc_debug_sound((int) c->keys, c->kind == CMD_BANKSOUND);
        } else if (c->kind == CMD_HIDETEXT && sFrame == c->frame) {
            extern int gMpHideText;     // 3ds/multiplayer_menu.inc

            gMpHideText = 1;
        } else if (c->kind == CMD_SKIPLOGOS && sFrame == c->frame) {
            gLogosSkippable = 1;
        } else if (c->kind == CMD_VOICECHECK && sFrame == c->frame) {
            extern void modchar_voices_check(void);     // characters.c

            modchar_voices_check();
        } else if (c->kind == CMD_NOMUSIC && sFrame == c->frame) {
            // The game's own music volume setting, down to nothing: music and
            // jingles go on playing, silently, and nothing else is disturbed.
            extern void music_volume_config_set(unsigned slider);
            extern void music_jingle_volume_set(unsigned char volume);

            music_volume_config_set(0);
            music_jingle_volume_set(0);
        } else if (c->kind == CMD_JINGLE && sFrame == c->frame) {
            pc_debug_jingle(0);
        } else if (c->kind == CMD_JINGLE_END && sFrame == c->frame) {
            pc_debug_jingle(1);
        } else if (c->kind == CMD_AUDIO && sFrame == c->frame) {
            pc_debug_audio_state();
        } else if (c->kind == CMD_EXIT && sFrame == c->frame) {
            printf("AUTOTEST: exit\n");
            fflush(stdout);
            exit(0);
        }
    }
}
