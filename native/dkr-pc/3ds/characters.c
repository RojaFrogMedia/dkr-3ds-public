// Added characters, and the bananas that unlock them.
//
// tools/import-characters.py turns character patches (.xdelta) into extra
// models in assets.bin and writes the list of them, characters.txt:
//
//     roster 99538e8e
//     Mario|9|360|361|362|363|920|144:913,156:914|mario.bin
//     name|donor|car|hover|plane|select|portrait|its own sounds (the game's id:its id)|its voice file
//
// This file reads that list and keeps the banana bank (bananas.txt): every
// banana the player picks up in any race is counted, and each
// MODCHAR_BANANAS_EACH of them unlocks the next character of the list, in
// the list's order. In a session between consoles every character of the
// list can be chosen, provided all the consoles have the same list
// (netplay_session_roster); with different lists there are none, because
// the consoles must run the same race.
//
// Where the game uses them:
//   src/menu.c              the character select (figures, cursor, names), the
//                           computer racers' characters, the result portraits
//   src/object_functions.c  the figures on the character select; bananas
//   src/objects.c           the racers' models
//   src/racer.c             the voice and horn (RACER_SOUND)
// A racer's added character is Racer.unk7 in the game's settings (0: none),
// next to Racer.character, which stays the donor's number; in a race it is
// Object_Racer.modCharacter.
//
// Voices: the importer adds each character's recordings to the game's sound
// bank as new sound ids, but puts the samples in a file per character,
// voices/<character>.bin; together they are too big to keep in memory. A
// recording is read in when it is first wanted (voice_want) and kept in a
// small cache; the audio thread takes its samples from there
// (modchar_voice_read, called by pc_dmacopy in reimpl.c). The rules that
// keep every voice with its own character are at "---- voices" below.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "characters.h"
#include "netplay.h"

static ModCharacter sCharacters[MODCHAR_MAX];
static int sCount;
static unsigned sRoster;
static int sBananas;
static int sSavedBananas;
static unsigned sPurchased;
static unsigned sSavedPurchased;
static int sNews;               // number of a character just unlocked
static char sBankPath[160];
static char sVoiceDir[160];
static LightLock sVoiceLock;

// "66:1020,68:1021": the game's sound ids and the character's own for them.
static void parse_sounds(ModCharacter *c, const char *text) {
    int from, to, used;

    c->soundCount = 0;
    while (c->soundCount < MODCHAR_MAX_SOUNDS && sscanf(text, "%d:%d%n", &from, &to, &used) == 2) {
        c->soundFrom[c->soundCount] = (short) from;
        c->soundTo[c->soundCount] = (short) to;
        c->soundCount++;
        text += used;
        if (*text == ',') {
            text++;
        }
    }
}

void modchar_init(const char *dir) {
    static char line[1024], sounds[800], voiceFile[80];
    char path[160];
    FILE *f;

    LightLock_Init(&sVoiceLock);
    snprintf(path, sizeof(path), "%s/characters.txt", dir);
    snprintf(sBankPath, sizeof(sBankPath), "%s/bananas.txt", dir);
    snprintf(sVoiceDir, sizeof(sVoiceDir), "%s/voices", dir);
    sCount = 0;
    sRoster = 0;
    f = fopen(path, "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL && sCount < MODCHAR_MAX) {
            ModCharacter *c = &sCharacters[sCount];

            if (sscanf(line, "roster %x", &sRoster) == 1) {
                continue;
            }
            sounds[0] = 0;
            voiceFile[0] = 0;
            memset(c, 0, sizeof(*c));
            if (line[0] != '#' &&
                sscanf(line, "%15[^|]|%d|%d|%d|%d|%d|%d|%799[^|\r\n]|%79[^|\r\n]", c->name, &c->donor, &c->object[0],
                       &c->object[1], &c->object[2], &c->selectObject, &c->portrait, sounds, voiceFile) >= 7 &&
                c->donor >= 0 && c->donor < 10) {
                // A list without the voice file's name is of the old kind, with
                // one voices.bin for everybody, in which the recordings were not
                // where the sound bank said: such characters have no voice here.
                if (voiceFile[0] != 0 && strcmp(voiceFile, "-") != 0 && strlen(voiceFile) < sizeof(c->voiceFile)) {
                    strcpy(c->voiceFile, voiceFile);
                    parse_sounds(c, sounds);
                } else if (strcmp(voiceFile, "-") != 0) {
                    printf("CHARACTERS: %s has no voice file in characters.txt (an old pack?); it stays silent\n",
                           c->name);
                }
                sCount++;
            }
        }
        fclose(f);
    }
    if (sCount == 0) {
        sRoster = 0;
    }
    f = fopen(sBankPath, "r");
    if (f != NULL) {
        if (fscanf(f, "bananas %d", &sBananas) != 1 || sBananas < 0) {
            sBananas = 0;
        }
        while (fgets(line, sizeof(line), f) != NULL) {
            if (sscanf(line, "purchased %x", &sPurchased) == 1) {
                // The roster may have changed; keep only valid entry bits.
                if (sCount < 32) sPurchased &= (1u << sCount) - 1u;
                break;
            }
        }
        fclose(f);
    }
    sSavedBananas = sBananas;
    sSavedPurchased = sPurchased;
    printf("CHARACTERS: %d added (roster %08x), %d bananas, %d unlocked\n", sCount, sRoster, sBananas,
           modchar_unlocked());
}

int modchar_count(void) {
    return sCount;
}

unsigned modchar_roster(void) {
    return sRoster;
}

int modchar_available(void) {
    if (netplay_players() != 0) {
        return (sRoster != 0 && netplay_session_roster() == sRoster) ? sCount : 0;
    }
    return modchar_unlocked();
}

int modchar_owned(int number) {
    return number >= 1 && number <= sCount && (sPurchased & (1u << (number - 1))) != 0;
}

// The characters that can be chosen are the ones bought, whichever they are
// (in a session: all of the list). The character select and the computer
// racers go through this; counting "the first so many of the list" instead
// is how a bought Shovel Knight once turned up as Bomberman.
int modchar_available_number(int index) {
    int number;

    if (index < 0) {
        return 0;
    }
    if (netplay_players() != 0) {
        return index < modchar_available() ? index + 1 : 0;
    }
    for (number = 1; number <= sCount; number++) {
        if (modchar_owned(number) && index-- == 0) {
            return number;
        }
    }
    return 0;
}

const ModCharacter *modchar_get(int number) {
    return (number >= 1 && number <= sCount) ? &sCharacters[number - 1] : NULL;
}

int modchar_racer_object(int number, int vehicle) {
    const ModCharacter *c = modchar_get(number);

    return (c != NULL && vehicle >= 0 && vehicle < 3) ? c->object[vehicle] : -1;
}

// ---- voices
//
// Whose voice plays is decided in one place, modchar_sound, by three rules:
//   1. One of the game's ten characters plays the game's own sound, as ever.
//   2. An added character plays a recording only out of its own voice file.
//      Each has one, voices/<file> (the last field of its line in
//      characters.txt), holding the recordings of its own lines and nobody
//      else's. A recording's address in the sound bank names the character
//      it belongs to and the place in that character's file; before one is
//      read it must belong to the character who is speaking and be where the
//      file's own list says it is (voice_want). Anything else is silence and
//      a line in the log, never somebody else's voice.
//   3. A line the patch has no recording for is never the donor's voice: for
//      a cheer or a groan the character's own nearest one is used, the two
//      lines of the character select stay silent. The horn is the exception:
//      it is a car's horn, not a voice, so the donor's sounds if the patch
//      brought none.
//
// The cache: recordings read from the voice files. The game thread fills it,
// the audio thread reads it, sVoiceLock between them.
#define VOICE_SLOTS 200                     // eight racers' voices and some
#define VOICE_BYTES (10 * 1024 * 1024)      // and no more memory than this
// A recording's address: the number of the character it belongs to, then
// the place in that character's voice file (tools/import-characters.py,
// class Sounds; pc_voice_offset in reimpl.c).
#define VOICE_OWNER_SHIFT 24
#define VOICE_PLACE_MASK ((1u << VOICE_OWNER_SHIFT) - 1)

extern int sound_voice_range(int soundId, unsigned *address, unsigned *length);    // src/audio.c
extern int sound_voice_line(int soundId, int *character, int *row);                // src/audio.c

static struct {
    unsigned address, length;
    unsigned char *samples;     // NULL: free
    unsigned used;              // sVoiceClock when last wanted
} sVoices[VOICE_SLOTS];
static unsigned sVoiceClock, sVoiceBytes;
static FILE *sVoiceFile;        // one voice file stays open: opening a file on the SD card costs more than the read
static int sVoiceFileNumber;    // whose it is
static unsigned char sSoundLogged[MODCHAR_MAX][MODCHAR_MAX_SOUNDS];
static int sVoiceRefusals;

// What a character's voice file says it holds: the list at its head.
typedef struct VoiceList {
    int state;                  // 0: not read yet, 1: read, -1: no usable file
    int count;
    struct {
        unsigned sound, place, length;
    } entry[MODCHAR_MAX_SOUNDS];
} VoiceList;
static VoiceList sVoiceLists[MODCHAR_MAX];

static FILE *voice_file(int number) {
    char path[260];

    if (sVoiceFile != NULL && sVoiceFileNumber == number) {
        return sVoiceFile;
    }
    if (sVoiceFile != NULL) {
        fclose(sVoiceFile);
    }
    snprintf(path, sizeof(path), "%s/%s", sVoiceDir, sCharacters[number - 1].voiceFile);
    sVoiceFile = fopen(path, "rb");
    sVoiceFileNumber = number;
    return sVoiceFile;
}

static VoiceList *voice_list(int number) {
    const ModCharacter *c = &sCharacters[number - 1];
    VoiceList *list = &sVoiceLists[number - 1];
    struct {
        char magic[4];
        u32 version, count;
        char name[16];
    } head;
    u32 entry[3];
    FILE *f;
    u32 i;

    if (list->state != 0) {
        return list->state > 0 ? list : NULL;
    }
    list->state = -1;
    if (c->voiceFile[0] == 0) {
        return NULL;    // a character without recordings
    }
    f = voice_file(number);
    if (f == NULL || fseek(f, 0, SEEK_SET) != 0 || fread(&head, sizeof(head), 1, f) != 1 ||
        memcmp(head.magic, "DKRV", 4) != 0 || head.version != 1 || head.count > MODCHAR_MAX_SOUNDS ||
        strncmp(head.name, c->name, sizeof(head.name)) != 0) {
        printf("CHARACTERS: voices/%s is not %s's voice file; %s stays silent\n", c->voiceFile, c->name, c->name);
        return NULL;
    }
    for (i = 0; i < head.count; i++) {
        if (fread(entry, sizeof(entry), 1, f) != 1) {
            printf("CHARACTERS: voices/%s is cut short; %s stays silent\n", c->voiceFile, c->name);
            return NULL;
        }
        list->entry[i].sound = entry[0];
        list->entry[i].place = entry[1];
        list->entry[i].length = entry[2];
    }
    list->count = (int) head.count;
    list->state = 1;
    return list;
}

static int voice_refuse(int number, int soundId, const char *why) {
    if (sVoiceRefusals++ < 40) {
        printf("CHARACTERS: %s does not play sound %d: %s\n", sCharacters[number - 1].name, soundId, why);
    }
    return 0;
}

// Has a recording of added character `number` in memory, for the audio
// thread to play. 0, and nothing to play, unless it is that character's own.
static int voice_want(int number, int soundId) {
    unsigned address, length;
    unsigned char *samples;
    VoiceList *list;
    FILE *f;
    int i, slot;

    if (number < 1 || number > sCount) {
        return 0;
    }
    if (!sound_voice_range(soundId, &address, &length) || length == 0) {
        return voice_refuse(number, soundId, "it is not a recording in a voice file");
    }
    if ((int) (address >> VOICE_OWNER_SHIFT) != number) {
        return voice_refuse(number, soundId, "it is another character's recording");
    }
    list = voice_list(number);
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->count; i++) {
        if (list->entry[i].sound == (unsigned) soundId) {
            break;
        }
    }
    if (i == list->count || list->entry[i].place != (address & VOICE_PLACE_MASK) || list->entry[i].length != length) {
        return voice_refuse(number, soundId, "the sound bank and its voice file disagree about it (assets.bin and "
                                             "the voices folder are not of the same pack)");
    }
    sVoiceClock++;
    for (i = 0; i < VOICE_SLOTS; i++) {
        if (sVoices[i].samples != NULL && sVoices[i].address == address) {
            sVoices[i].used = sVoiceClock;
            return 1;
        }
    }
    f = voice_file(number);
    samples = malloc(length);
    if (samples == NULL || f == NULL || fseek(f, (long) (address & VOICE_PLACE_MASK), SEEK_SET) != 0 ||
        fread(samples, 1, length, f) != length) {
        free(samples);
        return voice_refuse(number, soundId, "it could not be read from its voice file");
    }
    // Into a free slot, the longest unused ones making way if need be.
    LightLock_Lock(&sVoiceLock);
    for (;;) {
        int oldest = -1;

        slot = -1;
        for (i = 0; i < VOICE_SLOTS; i++) {
            if (sVoices[i].samples == NULL) {
                slot = i;
            } else if (oldest < 0 || sVoices[i].used < sVoices[oldest].used) {
                oldest = i;
            }
        }
        if ((slot >= 0 && sVoiceBytes + length <= VOICE_BYTES) || oldest < 0) {
            break;
        }
        free(sVoices[oldest].samples);
        sVoices[oldest].samples = NULL;
        sVoiceBytes -= sVoices[oldest].length;
    }
    sVoices[slot].address = address;
    sVoices[slot].length = length;
    sVoices[slot].samples = samples;
    sVoices[slot].used = sVoiceClock;
    sVoiceBytes += length;
    LightLock_Unlock(&sVoiceLock);
    return 1;
}

// For tests: a sound id played without a character (pc_debug_sound in
// src/audio.c). An added character's recording is read in for whoever owns it.
void modchar_voice_want_sound(int soundId) {
    unsigned address, length;

    if (sound_voice_range(soundId, &address, &length)) {
        voice_want((int) (address >> VOICE_OWNER_SHIFT), soundId);
    }
}

// The i-th of a character's own recordings, ready to play; 0 (no sound) if
// the patch made that line silent or the recording cannot be had.
static int voice_own(int number, int i) {
    const ModCharacter *c = &sCharacters[number - 1];

    if (c->soundTo[i] == 0) {
        return 0;
    }
    if (!sSoundLogged[number - 1][i]) {
        sSoundLogged[number - 1][i] = 1;
        printf("CHARACTERS: %s sound %d is its own %d\n", c->name, c->soundFrom[i], c->soundTo[i]);
    }
    return voice_want(number, c->soundTo[i]) ? c->soundTo[i] : 0;
}

int modchar_sound(int number, int soundId) {
    const ModCharacter *c = modchar_get(number);
    int i, kind, character, row, otherRow, nearest = -1, distance = 0;

    if (c == NULL) {
        return soundId;     // one of the game's ten: the game's own sound
    }
    for (i = 0; i < c->soundCount; i++) {
        if (c->soundFrom[i] == soundId) {
            return voice_own(number, i);
        }
    }
    // The patch has no recording for this.
    kind = sound_voice_line(soundId, &character, &row);
    if (kind == VOICE_LINE_NONE || kind == VOICE_LINE_HORN) {
        return soundId;     // an effect or the car's horn, nobody's voice: the donor's
    }
    if (kind == VOICE_LINE_POSITIVE || kind == VOICE_LINE_NEGATIVE) {
        // Its own nearest cheer or groan in place of the donor's.
        for (i = 0; i < c->soundCount; i++) {
            if (c->soundTo[i] != 0 && sound_voice_line(c->soundFrom[i], &character, &otherRow) == kind &&
                (nearest < 0 || abs(otherRow - row) < distance)) {
                nearest = i;
                distance = abs(otherRow - row);
            }
        }
        if (nearest >= 0) {
            return voice_own(number, nearest);
        }
    }
    return 0;
}

// For the log: sample reads the audio thread was given, and ones it asked
// for that were not in memory (played as silence).
static unsigned sVoiceReads, sVoiceMisses, sVoiceReported;

void modchar_voices_preload(int number) {
    const ModCharacter *c = modchar_get(number);
    int i, have = 0;

    for (i = 0; c != NULL && i < c->soundCount; i++) {
        if (c->soundTo[i] != 0) {
            have += voice_want(number, c->soundTo[i]);
        }
    }
    if (c != NULL) {
        printf("CHARACTERS: %s's voice in memory (%d recordings from voices/%s; %u KB of voices held)\n", c->name,
               have, c->voiceFile, sVoiceBytes / 1024);
    }
}

void modchar_voice_read(unsigned address, void *dst, int bytes) {
    int i;

    memset(dst, 0, bytes);
    LightLock_Lock(&sVoiceLock);
    for (i = 0; i < VOICE_SLOTS; i++) {
        if (sVoices[i].samples != NULL && address >= sVoices[i].address &&
            address < sVoices[i].address + sVoices[i].length) {
            unsigned have = sVoices[i].address + sVoices[i].length - address;

            memcpy(dst, sVoices[i].samples + (address - sVoices[i].address),
                   have < (unsigned) bytes ? have : (unsigned) bytes);
            break;
        }
    }
    if (i < VOICE_SLOTS) {
        sVoiceReads++;
    } else {
        sVoiceMisses++;
    }
    LightLock_Unlock(&sVoiceLock);
}

// For tests (the VOICECHECK command of 3ds/autotest.c): every recording of
// every added character, fetched the way a race fetches it and read the way
// the audio thread reads it, with a check sum per character in the log for
// tools/check-voices.py to compare with the patches' own recordings.
static u32 voice_crc(u32 crc, const unsigned char *data, int bytes) {
    static u32 table[256];
    int i, bit;

    if (table[1] == 0) {
        for (i = 0; i < 256; i++) {
            u32 value = (u32) i;

            for (bit = 0; bit < 8; bit++) {
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
            }
            table[i] = value;
        }
    }
    for (i = 0; i < bytes; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

void modchar_voices_check(void) {
    static unsigned char chunk[4096];
    int number, i, kept, wrong = 0, total = 0;

    for (number = 1; number <= sCount; number++) {
        const ModCharacter *c = &sCharacters[number - 1];
        u32 crc = 0xffffffffu;
        int good = 0, silent = 0, refused = 0;

        for (i = 0; i < c->soundCount; i++) {
            unsigned address, length, at;

            if (c->soundTo[i] == 0) {
                silent++;
            } else if (!voice_want(number, c->soundTo[i]) || !sound_voice_range(c->soundTo[i], &address, &length)) {
                refused++;
            } else {
                for (at = 0; at < length; at += sizeof(chunk)) {
                    int bytes = (int) (length - at < sizeof(chunk) ? length - at : sizeof(chunk));

                    modchar_voice_read(address + at, chunk, bytes);
                    crc = voice_crc(crc, chunk, bytes);
                }
                good++;
            }
        }
        printf("VOICECHECK: %s|%d|%d|%d|%08lx\n", c->name, good, silent, refused, (unsigned long) (crc ^ 0xffffffffu));
        wrong += refused;
        total += good;
    }
    // And the other way round: none of them may be given another's recording.
    // (voice_want would write a line for each refusal; these are meant.)
    kept = sVoiceRefusals;
    sVoiceRefusals = 1000;
    for (number = 1; number <= sCount; number++) {
        int other = number % sCount + 1;

        if (other != number && sCharacters[other - 1].soundCount > 0 && sCharacters[other - 1].soundTo[0] != 0 &&
            voice_want(number, sCharacters[other - 1].soundTo[0])) {
            printf("VOICECHECK: %s was given a recording of %s\n", sCharacters[number - 1].name,
                   sCharacters[other - 1].name);
            wrong++;
        }
    }
    sVoiceRefusals = kept;
    printf("VOICECHECK: %d recordings of %d characters read from their own files, %d wrong\n", total, sCount, wrong);
}

// ---- the banana bank

int modchar_bananas(void) {
    return sBananas;
}

int modchar_unlocked(void) {
    int i, count = 0;

    for (i = 0; i < sCount; i++) {
        if (sPurchased & (1u << i)) count++;
    }
    return count;
}

int modchar_purchase(int number) {
    int before = modchar_unlocked();

    if (number < 1 || number > sCount || (sPurchased & (1u << (number - 1))) ||
        sBananas < MODCHAR_BANANAS_EACH) {
        return 0;
    }
    sBananas -= MODCHAR_BANANAS_EACH;
    sPurchased |= 1u << (number - 1);
    sNews = number;
    modchar_save();
    printf("CHARACTERS: %s purchased for %d bananas (%d remain)\n", sCharacters[number - 1].name,
           MODCHAR_BANANAS_EACH, sBananas);
    return 1;
}

void modchar_add_banana(void) {
    sBananas++;
    // This runs in the race update when a banana is collected. Keep the
    // increment in memory; write the bank at menu transitions, HOME pause,
    // purchase, or shutdown, never synchronously on a pickup frame.
}

void modchar_save(void) {
    FILE *f;

    if (sVoiceReads + sVoiceMisses != sVoiceReported) {
        sVoiceReported = sVoiceReads + sVoiceMisses;
        printf("CHARACTERS: voices played from memory: %u reads, %u missed\n", sVoiceReads, sVoiceMisses);
    }
    if (sBananas == sSavedBananas && sPurchased == sSavedPurchased) {
        return;
    }
    f = fopen(sBankPath, "w");
    if (f != NULL) {
        fprintf(f, "bananas %d\npurchased %08x\n", sBananas, sPurchased);
        fclose(f);
        sSavedBananas = sBananas;
        sSavedPurchased = sPurchased;
    }
}

const char *modchar_take_news(void) {
    int news = sNews;

    sNews = 0;
    return news != 0 ? sCharacters[news - 1].name : NULL;
}
