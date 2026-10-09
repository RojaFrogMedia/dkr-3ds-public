// Audio on the 3DS.
//
// The audio manager is ticked from the frame loop (pc_audio_frame) until a
// few frames are queued, as on the other PC targets. A tick has two halves:
// the synthesizer walks its players and writes a command list (cheap, and
// tied to state the game also touches), then the list is mixed into samples
// (most of the cost, and private to the list). The mixing half runs on a
// worker thread on the second core; the finished samples go from there onto
// one NDSP channel through a ring of wave buffers. The next tick waits for
// the mix in flight, which keeps the order the N64 has, where the audio
// thread waits for the RSP before it builds another list.
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"

extern void am_audio_frame_pc(void);
extern unsigned int frameSize;

static FILE *sDump;                 // the test recording, see dump_samples
#define AUDIO_RATE 22050
#define BYTES_PER_FRAME 4           // stereo s16
#define NUM_BUFS 12
#define MAX_FRAMES_PER_BUF 1024

static ndspWaveBuf sBufs[NUM_BUFS];
static s16 *sStorage[NUM_BUFS];
static int sNext;
static int sAudioReady;
static u32 sDropped;

void pc_audio_hle_run_sync(void *cmdList, s32 cmdLen, void *out, s32 frameSamples);

// The menu's volume setting (touchmenu.c); it may arrive before the DSP is up.
static int sVolumePercent = 100;

void pc_audio_set_volume(int percent) {
    sVolumePercent = percent;
    if (sAudioReady) {
        ndspSetMasterVol((float) percent / 100.0f);
    }
}

static struct { void *cmdList; s32 cmdLen; void *out; s32 frameSamples; } sJob;
static volatile int sJobBusy;
static LightEvent sJobEvent, sIdleEvent;
static Thread sWorker;

static void queue_samples(void *buf, u32 size);

static void worker_main(void *arg) {
    (void) arg;
    diag_thread_init();
    for (;;) {
        gDiagAudioPhase = DIAG_AUDIO_IDLE;
        LightEvent_Wait(&sJobEvent);
        gDiagAudioPhase = DIAG_AUDIO_MIX;
        pc_audio_hle_run_sync(sJob.cmdList, sJob.cmdLen, sJob.out, sJob.frameSamples);
        gDiagAudioPhase = DIAG_AUDIO_QUEUE;
        queue_samples(sJob.out, (u32) sJob.frameSamples * BYTES_PER_FRAME);
        sJobBusy = 0;
        LightEvent_Signal(&sIdleEvent);
    }
}

static void wait_for_mix(void) {
    if (sJobBusy) {
        int phase = gDiagMainPhase;

        gDiagMainPhase = DIAG_MAIN_WAIT_MIX;
        while (sJobBusy) {
            LightEvent_Wait(&sIdleEvent);
        }
        gDiagMainPhase = phase;
    }
}

// The same, giving up after a second (the HOME button, sleep, leaving: see
// render_wait_idle_bounded in platform.c). FALSE: gave up.
int pc_audio_wait_idle_bounded(void) {
    int tries;

    for (tries = 0; sJobBusy && tries < 20; tries++) {
        LightEvent_WaitTimeout(&sIdleEvent, 50000000ll);
    }
    if (sJobBusy) {
        diag_trail("the sound mixer did not finish its list");
        return 0;
    }
    return 1;
}

// Returns once the mixer has finished the list it was given.
void pc_audio_wait_idle(void) {
    wait_for_mix();
}

// What the audio manager calls with a finished command list.
void pc_audio_hle_run(void *cmdList, s32 cmdLen, void *out, s32 frameSamples) {
    if (sWorker == NULL) {
        pc_audio_hle_run_sync(cmdList, cmdLen, out, frameSamples);
        queue_samples(out, (u32) frameSamples * BYTES_PER_FRAME);
        return;
    }
    wait_for_mix();
    sJob.cmdList = cmdList;
    sJob.cmdLen = cmdLen;
    sJob.out = out;
    sJob.frameSamples = frameSamples;
    sJobBusy = 1;
    LightEvent_Signal(&sJobEvent);
}

// Stops the DSP; called once, when the program leaves.
void pc_audio_shutdown(void) {
    if (sDump != NULL) {
        fclose(sDump);
        sDump = NULL;
    }
    if (sAudioReady) {
        pc_audio_wait_idle_bounded();
        sAudioReady = 0;
        ndspChnWaveBufClear(0);
        ndspExit();
    }
}

s32 osAiSetFrequency(u32 frequency) {
    int i;

    if (sAudioReady) {
        return AUDIO_RATE;
    }
    if (R_FAILED(ndspInit())) {
        // No DSP firmware dump (sdmc:/3ds/dspfirm.cdc): the game runs silent.
        printf("AUDIO: ndspInit failed, running silent\n");
        return (s32) frequency;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float) AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    for (i = 0; i < NUM_BUFS; i++) {
        sStorage[i] = linearAlloc(MAX_FRAMES_PER_BUF * BYTES_PER_FRAME);
        memset(&sBufs[i], 0, sizeof(sBufs[i]));
        sBufs[i].status = NDSP_WBUF_FREE;
    }
    // The second core is the system's; an application may take up to 80%.
    LightEvent_Init(&sJobEvent, RESET_ONESHOT);
    LightEvent_Init(&sIdleEvent, RESET_ONESHOT);
    APT_SetAppCpuTimeLimit(80);
    sWorker = threadCreate(worker_main, NULL, 32 * 1024, 0x18, 1, false);
    if (sWorker == NULL) {
        printf("AUDIO: no worker thread, mixing on the main thread\n");
    }
    sAudioReady = 1;
    pc_audio_set_volume(sVolumePercent);
    return AUDIO_RATE;
}

static u32 queued_bytes(void) {
    u32 frames = 0;
    int i;

    if (!sAudioReady) {
        return 0;
    }
    for (i = 0; i < NUM_BUFS; i++) {
        if (sBufs[i].status == NDSP_WBUF_QUEUED) {
            frames += sBufs[i].nsamples;
        } else if (sBufs[i].status == NDSP_WBUF_PLAYING) {
            u32 pos = ndspChnGetSamplePos(0);
            frames += pos < sBufs[i].nsamples ? sBufs[i].nsamples - pos : 0;
        }
    }
    return frames * BYTES_PER_FRAME;
}

// The game hands over each frame one tick after it was mixed; the worker
// has queued it by then.
void osAiSetNextBuffer(void *buf, u32 size) {
    (void) buf; (void) size;
}

// A recording of everything the game mixes, for tests: with an empty file
// AUDIODUMP.TXT in the game's folder, the samples (22050 Hz, stereo, 16 bit)
// are also written to audio.raw there. tools/run-native.sh sets it up with
// DKR_AUDIODUMP=1 and tools/audio-check.py reads the result.
static int sDumpChecked;

static void dump_samples(void *buf, u32 bytes) {
    if (!sDumpChecked) {
        FILE *wanted = fopen("sdmc:/3ds/DKR/AUDIODUMP.TXT", "r");

        sDumpChecked = 1;
        if (wanted != NULL) {
            fclose(wanted);
            sDump = fopen("sdmc:/3ds/DKR/audio.raw", "wb");
        }
    }
    if (sDump != NULL) {
        fwrite(buf, 1, bytes, sDump);
    }
}

static void queue_samples(void *buf, u32 size) {
    ndspWaveBuf *wb;
    u32 frames = size / BYTES_PER_FRAME;

    if (!sAudioReady || buf == NULL || frames == 0) {
        return;
    }
    dump_samples(buf, frames * BYTES_PER_FRAME);
    if (frames > MAX_FRAMES_PER_BUF) {
        frames = MAX_FRAMES_PER_BUF;
    }
    wb = &sBufs[sNext];
    if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
        sDropped++;         // the ring is full: the game is ahead of the DSP
        return;
    }
    memcpy(sStorage[sNext], buf, frames * BYTES_PER_FRAME);
    DSP_FlushDataCache(sStorage[sNext], frames * BYTES_PER_FRAME);
    wb->data_pcm16 = sStorage[sNext];
    wb->nsamples = frames;
    wb->looping = false;
    ndspChnWaveBufAdd(0, wb);
    sNext = (sNext + 1) % NUM_BUFS;
}

// The game sizes its next frame from this as (frame size - what is left), and
// that sum goes negative for more than one frame's worth.
u32 osAiGetLength(void) {
    u32 used = queued_bytes() + (sJobBusy ? (u32) sJob.frameSamples * BYTES_PER_FRAME : 0);
    u32 oneFrame;

    if (frameSize == 0) {
        return used;
    }
    oneFrame = frameSize * BYTES_PER_FRAME;
    return used > oneFrame ? oneFrame : used;
}

#define AUDIO_TARGET_FRAMES 3       // about 100 ms queued
#define AUDIO_MAX_TICKS 4

void pc_audio_frame(void) {
    u32 target;
    int ticks = 0;

    if (!sAudioReady || frameSize == 0) {
        return;
    }
    target = frameSize * BYTES_PER_FRAME * AUDIO_TARGET_FRAMES;
    while (queued_bytes() + (sJobBusy ? (u32) sJob.frameSamples * BYTES_PER_FRAME : 0) < target && ticks < AUDIO_MAX_TICKS) {
        wait_for_mix();
        am_audio_frame_pc();
        ticks++;
    }
}

void pc_audio_report(void) {
    static u32 lastDropped, lastEvents, lastParams;
    extern u32 gAlEvtqDropped;      // libultra/src/audio/event.c
    extern u32 gAlParamDropped;     // libultra/src/audio/mips1/synthesizer.c

    if (gAlParamDropped != lastParams) {
        printf("AUDIO: %lu voice updates dropped (pool empty)\n", (unsigned long) gAlParamDropped);
        lastParams = gAlParamDropped;
    }

    if (gAlEvtqDropped != lastEvents) {
        printf("AUDIO: %lu sequencer events dropped (queue full)\n", (unsigned long) gAlEvtqDropped);
        lastEvents = gAlEvtqDropped;
    }

    if (sDropped != lastDropped) {
        printf("AUDIO: %lu buffers dropped\n", (unsigned long) sDropped);
        lastDropped = sDropped;
    }
}
