// What went wrong, written to log.txt by the game itself.
//
//   CRASH   a thread took a processor exception: its kind, the address it
//           happened at and the caller's. `arm-none-eabi-addr2line -e
//           dkracing.elf <pc>` (or the .map file) turns those into a function.
//   STALL   no frame for three seconds: where the game thread and the render
//           thread each were, as the phases below.
//
// The phases are plain counters the threads set as they go, so reading them
// costs nothing while the game runs.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

void pc_log_sync(void);    // autotest.c: the log so far, on the card before this returns

volatile int gDiagMainPhase, gDiagRenderPhase, gDiagAudioPhase;
volatile unsigned gDiagMainFrames, gDiagRenderFrames;
volatile int gDiagPaused;

static const char *const sMainPhases[] = { "starting", "game logic", "waiting for the render thread", "audio tick",
                                           "waiting for the audio mixer", "pacing sleep", "controller poll" };
static const char *const sRenderPhases[] = { "idle", "frame begin (waiting for the GPU)", "interpreting the display list",
                                             "frame end (submitting to the GPU)", "texture upload" };
static const char *const sAudioPhases[] = { "idle", "mixing", "queueing samples" };

static const char *phase_name(const char *const *names, int count, int phase) {
    return (phase >= 0 && phase < count) ? names[phase] : "?";
}

// ---- the game thread's call stack (TRACE=1 builds only)
//
// With -finstrument-functions the compiler calls these two on entry to and
// exit from every function of the game and libultra. They keep the game
// thread's stack of function addresses, which a STALL report prints.

#define TRACE_DEPTH 48
static void *volatile sTrace[TRACE_DEPTH];
static volatile int sTraceDepth;
static void *sTraceThread;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site) {
    (void) site;
    if (getThreadLocalStorage() == sTraceThread) {
        int depth = sTraceDepth++;

        if (depth >= 0 && depth < TRACE_DEPTH) {
            sTrace[depth] = fn;
        }
    }
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) {
    (void) fn; (void) site;
    if (getThreadLocalStorage() == sTraceThread) {
        sTraceDepth--;
    }
}

static void report_trace(void) {
    int depth = sTraceDepth, i;

    if (depth <= 0) {
        return;
    }
    printf("  game thread call stack, outermost first (addr2line -f -e dkracing.elf <address>):\n   ");
    for (i = 0; i < depth && i < TRACE_DEPTH; i++) {
        printf(" %08lx", (unsigned long) sTrace[i]);
    }
    printf("\n");
}

static void report_phases(void) {
    printf("  game thread: %s (frame %u)\n  render thread: %s (frame %u)\n  audio mixer: %s\n",
           phase_name(sMainPhases, 7, gDiagMainPhase), gDiagMainFrames,
           phase_name(sRenderPhases, 5, gDiagRenderPhase), gDiagRenderFrames, phase_name(sAudioPhases, 3, gDiagAudioPhase));
    printf("  memory: heap free %lu KB, linear free %lu KB\n", (unsigned long) (envGetHeapSize() / 1024),
           (unsigned long) (linearSpaceFree() / 1024));
    report_trace();
    fflush(stdout);
    pc_log_sync();
}

// ---- exceptions

static u8 sExceptionStack[4][0x1000];
static int sExceptionStacks;

static void exception_handler(ERRF_ExceptionInfo *info, CpuRegisters *regs) {
    static const char *const kinds[] = { "prefetch abort", "data abort", "undefined instruction", "VFP exception" };

    printf("CRASH: %s at pc %08lx (lr %08lx, sp %08lx), fault address %08lx\n",
           info->type < 4 ? kinds[info->type] : "exception", (unsigned long) regs->pc, (unsigned long) regs->lr,
           (unsigned long) regs->sp, (unsigned long) info->far);
    printf("  r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r12 %08lx\n", (unsigned long) regs->r[0],
           (unsigned long) regs->r[1], (unsigned long) regs->r[2], (unsigned long) regs->r[3], (unsigned long) regs->r[4],
           (unsigned long) regs->r[5], (unsigned long) regs->r[12]);
    report_phases();
    svcExitProcess();
}

// Each thread that should report its own crashes calls this once.
void diag_thread_init(void) {
    if (sExceptionStacks < 4) {
        u8 *stack = sExceptionStack[sExceptionStacks++];

        threadOnException(exception_handler, stack + sizeof(sExceptionStack[0]), WRITE_DATA_TO_HANDLER_STACK);
    }
}

// ---- stalls

static void watchdog_main(void *arg) {
    unsigned lastMain = 0, lastRender = 0;
    int quiet = 0, reported = 0;

    (void) arg;
    for (;;) {
        svcSleepThread(500000000ll);
        if (gDiagPaused) {
            quiet = 0;
            continue;
        }
        if (gDiagMainFrames != lastMain || gDiagRenderFrames != lastRender) {
            lastMain = gDiagMainFrames;
            lastRender = gDiagRenderFrames;
            if (reported) {
                printf("STALL: running again\n");
            }
            quiet = reported = 0;
            continue;
        }
        if (++quiet == 6 && !reported) {
            reported = 1;
            printf("STALL: no frame for 3 seconds\n");
            report_phases();
        } else if (quiet == 8 && reported == 1) {
            // A second look: a loop shows as a different innermost call.
            reported = 2;
            report_trace();
            fflush(stdout);
            pc_log_sync();
        }
    }
}

// ---- the trail

static char sTrailPath[160];
static char sLastTrail[96];

void diag_trail_start(const char *dir) {
    char log[160], kept[160];
    FILE *f;

    snprintf(sTrailPath, sizeof(sTrailPath), "%s/state.txt", dir);
    f = fopen(sTrailPath, "r");
    if (f != NULL) {
        if (fgets(sLastTrail, sizeof(sLastTrail), f) == NULL) {
            sLastTrail[0] = 0;
        }
        fclose(f);
        sLastTrail[strcspn(sLastTrail, "\r\n")] = 0;
    }
    // The log of a run that did not end is evidence: it is kept as
    // log-prev.txt instead of being written over.
    if (sLastTrail[0] != 0 && strcmp(sLastTrail, "ended") != 0) {
        snprintf(log, sizeof(log), "%s/log.txt", dir);
        snprintf(kept, sizeof(kept), "%s/log-prev.txt", dir);
        remove(kept);
        rename(log, kept);
    }
}

void diag_trail(const char *step) {
    FILE *f;

    if (sTrailPath[0] == 0) {
        return;
    }
    f = fopen(sTrailPath, "w");
    if (f != NULL) {
        fputs(step, f);
        fclose(f);
    }
    printf("TRAIL: %s\n", step);
    fflush(stdout);
    pc_log_sync();
}

void diag_init(void) {
    if (sLastTrail[0] != 0 && strcmp(sLastTrail, "ended") != 0) {
        printf("LAST RUN: did not end; it stopped at or after \"%s\". Its log is log-prev.txt\n", sLastTrail);
    }
    diag_trail("running");
    sTraceThread = getThreadLocalStorage();
    diag_thread_init();
    // Above the game thread, asleep nearly all the time. 0x18 is the highest
    // priority an application is allowed; asking for more fails.
    if (threadCreate(watchdog_main, NULL, 16 * 1024, 0x18, 0, true) == NULL) {
        printf("DIAG: no watchdog thread\n");
    }
}
