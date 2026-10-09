// Crash and stall reports (diag.c). Includes nothing so that the files built
// with the game's own type names can use it too.
#ifndef DKR_3DS_DIAG_H
#define DKR_3DS_DIAG_H

enum { DIAG_MAIN_START, DIAG_MAIN_LOGIC, DIAG_MAIN_WAIT_RENDER, DIAG_MAIN_AUDIO, DIAG_MAIN_WAIT_MIX, DIAG_MAIN_SLEEP,
       DIAG_MAIN_INPUT };
enum { DIAG_RENDER_IDLE, DIAG_RENDER_BEGIN, DIAG_RENDER_INTERPRET, DIAG_RENDER_END, DIAG_RENDER_TEXTURE };
enum { DIAG_AUDIO_IDLE, DIAG_AUDIO_MIX, DIAG_AUDIO_QUEUE };

extern volatile int gDiagMainPhase, gDiagRenderPhase, gDiagAudioPhase;
extern volatile unsigned gDiagMainFrames, gDiagRenderFrames;
// Set while the program is suspended (HOME menu, sleep): no frames is right then.
extern volatile int gDiagPaused;

void diag_init(void);

// The trail: one line in state.txt saying what the program was last doing,
// written and closed at once so that it is on the card whatever follows.
// For the rare, risky moments (the HOME button, sleep, leaving), not for
// frames. The next start reports a run that did not get to its end.
void diag_trail_start(const char *dir);     // before the log is opened: keeps the last run's log and state
void diag_trail(const char *step);
void diag_thread_init(void);

#endif
