#ifndef DKR_3DS_CHARACTERS_H
#define DKR_3DS_CHARACTERS_H

// Added characters, and the bananas that unlock them. See characters.c.

#define MODCHAR_MAX 32                  // added characters at most
#define MODCHAR_MAX_SOUNDS 64           // recordings of its own per character at most
#define MODCHAR_BANANAS_EACH 150       // bananas for each one (also in tools/import-characters.py)

// One added character. It drives like its donor, one of the game's ten (the
// character number: 0 Krunch .. 9 Diddy), and has the donor's engine sound;
// what is its own are the models, the portrait and the voice.
typedef struct ModCharacter {
    char name[16];
    int donor;
    int object[3];      // object ids of its car, hovercraft and plane
    int selectObject;   // object id of its figure on the character select
    int portrait;       // 2D texture
    // Its own recordings: the game's sound id soundFrom[i] plays as soundTo[i]
    // (0: the line is silent), all of them in its own file voices/<voiceFile>.
    int soundCount;
    short soundFrom[MODCHAR_MAX_SOUNDS], soundTo[MODCHAR_MAX_SOUNDS];
    char voiceFile[48];
} ModCharacter;

// What a sound id of the game is to the character it belongs to
// (sound_voice_line in src/audio.c).
enum {
    VOICE_LINE_NONE,        // not a character's own line
    VOICE_LINE_SELECT,      // on the character select: under the cursor ("I'm Diddy")
    VOICE_LINE_DESELECT,    // on the character select: un-chosen
    VOICE_LINE_HORN,
    VOICE_LINE_POSITIVE,    // the eight cheers
    VOICE_LINE_NEGATIVE     // the eight groans
};

void modchar_init(const char *dir);

// Added characters are numbered from 1; 0 means "one of the game's own".
int modchar_count(void);                        // in characters.txt
int modchar_available(void);                    // how many can be chosen now
int modchar_available_number(int index);        // which they are: the number of the index-th (from 0), or 0
int modchar_owned(int number);                  // bought in the store
const ModCharacter *modchar_get(int number);    // NULL for 0 or a number out of range
int modchar_racer_object(int number, int vehicle);  // -1: use the game's own
unsigned modchar_roster(void);                  // identifies the set; 0 without one

// Voices. An added character's recordings are in its own file,
// voices/<voiceFile>, and are read into memory when wanted (on the game
// thread) for the audio thread to play. modchar_sound is the one place that
// decides whose voice a racer or a character select figure has.
int modchar_sound(int number, int soundId);     // the sound to play for that character (0: none); reads it in
void modchar_voices_preload(int number);        // reads in all of a character's recordings
void modchar_voice_read(unsigned address, void *dst, int bytes);    // audio thread: samples, or silence
void modchar_voices_check(void);                // tests: every recording of every character, into the log

// The banana bank: every banana this console's player picks up, ever.
int modchar_bananas(void);
int modchar_unlocked(void);                     // characters the bank has paid for
int modchar_purchase(int number);               // spend 150 bananas to unlock this character
void modchar_add_banana(void);
void modchar_save(void);                        // writes the bank if it changed
const char *modchar_take_news(void);            // the name of a character just unlocked, once; else NULL

#endif
