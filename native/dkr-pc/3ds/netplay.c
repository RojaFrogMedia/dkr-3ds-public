// Local Play between consoles: every console runs the whole game and the
// only thing exchanged is each player's controller, one sample per poll.
//
// The game is deterministic given the same build, the same save data, the
// same random seed, the same logic rate and the same controller samples, so
// the consoles stay in step as long as each poll on each console sees the
// same four pads. A poll therefore does not return until it has every
// player's sample for that poll number; a local sample is used DELAY polls
// after it was read, which is the time the others have to arrive.
//
// Before a session there is a lobby, as in Mario Kart DS: one console hosts
// a game, the others search, see it listed and join, everybody sees the list
// of players, and the host starts. The Local Play screen (localplay_menu.inc,
// part of the game's menu code) draws it and calls the netplay_lobby_*
// functions here. Packets travel through link.c.
//
// The host is player 1; joiners take the next slots in the order they
// arrived. Each joiner sends its samples to the host and the host passes
// everybody's on.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "netplay.h"
#include "characters.h"

#define MAGIC 0x33524B44u       // "DKR3"
#define PROTOCOL 5              // consoles must agree on this to play together
#define MAX_PLAYERS NETPLAY_MAX_PLAYERS
#define WINDOW 256              // polls remembered per player
#define REDUNDANCY 8            // samples per packet: covers lost packets
#define NAME_BYTES (NETPLAY_NAME_CHARS + 2)
#define DELAY_POLLS 3           // the least a sample waits before it is used
#define MAX_DELAY_POLLS 20

#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000)
#define HELLO_EVERY_MS 150      // lobby upkeep, both ways
#define MEMBER_TIMEOUT_MS 2500  // a joiner or host not heard from is gone
#define START_TIMEOUT_MS 6000
#define SESSION_TIMEOUT_MS 8000 // a poll waits this long for the others

enum { PKT_HELLO = 1, PKT_LOBBY, PKT_START, PKT_READY, PKT_INPUT, PKT_BYE };

typedef struct {
    u16 buttons;
    s8 x, y;
} NetInput;

typedef struct {
    u32 magic;
    u8 type;
    u8 player;      // INPUT: whose samples
    u8 count;       // INPUT: samples in the packet
    u8 pad;
    u32 frame;      // INPUT: poll number of the first sample
    u32 checkFrame; // INPUT: a poll number and the sender's state check there
    u32 check;
    NetInput in[REDUNDANCY];
} InputPacket;

// HELLO (joiner to host), LOBBY and START (host to joiner), READY, BYE.
typedef struct {
    u32 magic;
    u8 type;
    u8 slot;        // LOBBY, START: the receiver's slot
    u8 count;       // LOBBY, START: players
    u8 isNew;       // HELLO: the sender is a New 3DS
    u8 delay, rate; // START
    u8 protocol;
    u8 pad;
    u32 seed;       // START
    u32 stamp;      // HELLO: the sender's clock (ms); LOBBY: the receiver's last one, sent back
    u16 rttMs;      // HELLO: the round trip the sender last measured that way
    u16 pad2;
    u32 roster;     // HELLO: the sender's set of added characters (3ds/characters.c);
                    // START: the set the session plays with, 0 for none
    s16 ratings[MAX_PLAYERS];               // HELLO: [0] is the sender's (points: they go below zero)
    char names[MAX_PLAYERS][NAME_BYTES];    // HELLO: [0] is the sender's
} LobbyPacket;

// What a searching console is shown about a game (link.c carries it).
typedef struct {
    u32 magic;
    u8 protocol;
    u8 players;
    u8 pad[2];
    char name[NAME_BYTES];
    u8 rest[LINK_INFO_BYTES - 8 - NAME_BYTES];
} GameInfo;

// ---- this console

static char sDir[128];
static char sMyName[NAME_BYTES] = "PLAYER";
static bool sIsNew;

// The console's user name, as far as the game's font can show it.
static void read_my_name(void) {
    u16 utf16[0x1C / 2] = { 0 };
    int i, n = 0;

    if (R_FAILED(cfguInit())) {
        return;
    }
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, utf16))) {
        for (i = 0; i < 10 && utf16[i] != 0 && n < NETPLAY_NAME_CHARS; i++) {
            u16 c = utf16[i];

            if (c >= 'a' && c <= 'z') {
                c = (u16) (c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') {
                sMyName[n++] = (char) c;
            }
        }
        if (n > 0) {
            sMyName[n] = '\0';
        }
    }
    cfguExit();
}

// ---- the player's profile and points
//
// Made the first time the player opens Multiplayer, in profile.txt in the
// game's folder: the name the others see and the points that races move up
// and down, in the way of Mario Kart Wii's VR as Retro Rewind plays it.
// Everybody starts at 0 and the points can go below it. There is no server,
// so the points are kept by the console they belong to; what keeps them
// honest between honest players is that every console of a session works
// out the same changes for everybody from the same results
// (netplay_report_race). The code calls them "rating" throughout.

#define RATING_START 0
#define RATING_MIN (-9999)
#define RATING_MAX 30000

static int sMyRating = RATING_START;
static int sMyRaces;
static int sSessionRatings[MAX_PLAYERS];    // everybody's, as the session goes
static int sRatingChange[MAX_PLAYERS];      // what the last race did to them

static void profile_save(void) {
    char path[160];
    FILE *f;

    snprintf(path, sizeof(path), "%s/profile.txt", sDir);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "# Diddy Kong Racing 3DS multiplayer profile\nname %s\npoints %d\nraces %d\n", sMyName, sMyRating, sMyRaces);
    fclose(f);
}

// Reads the profile, or makes one with the console's user name.
static void profile_load(void) {
    char path[160], line[96], word[16], value[32];
    FILE *f;

    snprintf(path, sizeof(path), "%s/profile.txt", sDir);
    f = fopen(path, "r");
    if (f == NULL) {
        profile_save();
        printf("NETPLAY: profile made for %s\n", sMyName);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%15s %31[^\r\n]", word, value) != 2) {
            continue;
        }
        // A file from before the points began at 0 has "rating" (5000 to
        // start with) instead: that is left behind and the points begin anew.
        if (strcmp(word, "points") == 0 && atoi(value) >= RATING_MIN && atoi(value) <= RATING_MAX) {
            sMyRating = atoi(value);
        } else if (strcmp(word, "races") == 0 && atoi(value) >= 0) {
            sMyRaces = atoi(value);
        }
    }
    fclose(f);
}

// ---- the lobby

static int sLobby = LOBBY_OFF;
static int sOnline;             // the lobby is an online one (a code, no search)
static u64 sLastUpkeep;
static u32 sMyRttMs;            // joiner: round trip to the host, from the lobby's upkeep
static u32 sLastStampSeen;
static char sCode[20];          // host, online: the game's code as shown

// Host: the joiners, by link node (1..3). Slot order is node order.
static struct {
    int present, isNew, ready;
    u64 heard;
    u32 stamp;      // the clock value of its last HELLO, to send back
    u32 rttMs;      // the round trip it reports
    s16 rating;
    u32 roster;     // its set of added characters
    char name[NAME_BYTES];
} sMembers[LINK_MAX_NODES];
static u32 sSessionRoster;      // the set of added characters every console of the session has, or 0

// Both: the player list as last built (host) or received (joiner).
static int sListCount;
static char sListNames[MAX_PLAYERS][NAME_BYTES];
static s16 sListRatings[MAX_PLAYERS];
static int sListSlot;
static u64 sHostHeard;          // joiner: when the host last spoke
static u64 sStartBegan;

static LinkGame sGames[LINK_MAX_GAMES];
static int sGameCount;

// ---- the session

static int sPlayers;            // 0: no session
static int sSlot;               // this console's player
static int sIsHost;
static int sDelay = DELAY_POLLS;
static int sRate = 1;
static u32 sSeed;
static u32 sPoll;               // the next poll's number
static int sSlotNode[MAX_PLAYERS];                  // host: the link node of each player
static NetInput sInputs[MAX_PLAYERS][WINDOW];
static u32 sHave[MAX_PLAYERS][WINDOW];              // poll number + 1 of what the slot holds
static u32 sLatest[MAX_PLAYERS];                    // newest poll number known per player, + 1
static int sLost;
// Players taken out of the session by the game (netplay_drop_player): their
// samples are no longer waited for and read as a pad left alone.
static u8 sDropped[MAX_PLAYERS];
static int sIdleWarning;        // the player the touch screens warn about, 1-based; 0: nobody

// State checks: what this console and the others computed at the same poll.
static u32 sCheckFrame, sCheck;
static u32 sMyChecks[WINDOW];
static int sDesyncReported;
static int sVerbose;
extern u32 pc_session_state_check(void);            // the game's side (localplay_menu.inc)

// ---- the code of an online game
//
// The host's address and port with a check byte, 56 bits as twelve letters
// and digits (none that look alike), shown in three groups of four. It is
// the address, not a ticket from a server: whoever has it can reach the
// host, and the host's console is all there is on the other end.
static int sCodePublic;
static const char sCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const u8 sCodeMask[6] = { 0x5D, 0xA3, 0x17, 0xC9, 0x62, 0x8E };

static u8 code_check(const u8 bytes[6]) {
    u32 sum = 0x3B;
    int i;

    for (i = 0; i < 6; i++) {
        sum = sum * 31 + bytes[i];
    }
    return (u8) (sum ^ (sum >> 8));
}

static void code_from_address(unsigned address, int port, char out[20]) {
    u8 bytes[7];
    u64 bits = 0;
    int i, n = 0;

    memcpy(bytes, &address, 4);
    bytes[4] = (u8) (port >> 8);
    bytes[5] = (u8) port;
    bytes[6] = code_check(bytes);
    for (i = 0; i < 6; i++) {
        bytes[i] ^= sCodeMask[i];
    }
    for (i = 0; i < 7; i++) {
        bits = (bits << 8) | bytes[i];
    }
    for (i = 0; i < 12; i++) {
        if (i == 4 || i == 8) {
            out[n++] = '-';
        }
        out[n++] = sCodeAlphabet[((bits << 4) >> (55 - 5 * i)) & 31];
    }
    out[n] = '\0';
}

static int code_to_address(const char *text, unsigned *address, int *port) {
    u8 bytes[7];
    u64 bits = 0;
    int i, count = 0;

    for (; *text != '\0'; text++) {
        char c = *text;
        const char *at;

        if (c == '-' || c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char) (c - 'a' + 'A');
        }
        at = strchr(sCodeAlphabet, c);
        if (at == NULL || count >= 12) {
            return 0;
        }
        bits = (bits << 5) | (u64) (at - sCodeAlphabet);
        count++;
    }
    if (count != 12) {
        return 0;
    }
    // Twelve characters are 60 bits; the code is the 56 that the encoder
    // wrote above its last character's low bit position.
    bits >>= 4;
    for (i = 6; i >= 0; i--) {
        bytes[i] = (u8) bits;
        bits >>= 8;
    }
    for (i = 0; i < 6; i++) {
        bytes[i] ^= sCodeMask[i];
    }
    if (bytes[6] != code_check(bytes)) {
        return 0;
    }
    memcpy(address, bytes, 4);
    *port = (bytes[4] << 8) | bytes[5];
    return 1;
}

static u64 ms_since(u64 then) {
    return (svcGetSystemTick() - then) / TICKS_PER_MS;
}

static void copy_name(char *dst, const char *src) {
    strncpy(dst, src, NAME_BYTES - 1);
    dst[NAME_BYTES - 1] = '\0';
}

static void make_info(GameInfo *info, int players) {
    memset(info, 0, sizeof(*info));
    info->magic = MAGIC;
    info->protocol = PROTOCOL;
    info->players = (u8) players;
    copy_name(info->name, sMyName);
}

static void send_lobby_packet(int node, int type, int slot) {
    LobbyPacket p;
    int i;

    memset(&p, 0, sizeof(p));
    p.magic = MAGIC;
    p.type = (u8) type;
    p.protocol = PROTOCOL;
    p.slot = (u8) slot;
    p.count = (u8) sListCount;
    p.isNew = (u8) sIsNew;
    p.delay = (u8) sDelay;
    p.rate = (u8) sRate;
    p.seed = sSeed;
    p.roster = sSessionRoster;
    if (type == PKT_HELLO) {
        p.roster = modchar_roster();
        copy_name(p.names[0], sMyName);
        p.ratings[0] = (s16) sMyRating;
        p.stamp = (u32) (svcGetSystemTick() / TICKS_PER_MS) | 1;
        p.rttMs = (u16) (sMyRttMs > 60000 ? 60000 : sMyRttMs);
    } else {
        if (node >= 1 && node < LINK_MAX_NODES) {
            p.stamp = sMembers[node].stamp;
        }
        for (i = 0; i < MAX_PLAYERS; i++) {
            copy_name(p.names[i], sListNames[i]);
            p.ratings[i] = sListRatings[i];
        }
    }
    link_send(node, &p, sizeof(p));
}

// Host: the player list from who is present, and each member's slot.
static void host_build_list(int slotOfNode[LINK_MAX_NODES]) {
    int node;

    sListCount = 1;
    sListSlot = 0;
    copy_name(sListNames[0], sMyName);
    sListRatings[0] = (s16) sMyRating;
    for (node = 1; node < LINK_MAX_NODES; node++) {
        slotOfNode[node] = -1;
        if (sMembers[node].present && sListCount < MAX_PLAYERS) {
            slotOfNode[node] = sListCount;
            copy_name(sListNames[sListCount], sMembers[node].name);
            sListRatings[sListCount] = sMembers[node].rating;
            sListCount++;
        }
    }
}

static void session_begin(int players, int slot, int isHost) {
    memset(sHave, 0, sizeof(sHave));
    memset(sLatest, 0, sizeof(sLatest));
    memset(sMyChecks, 0, sizeof(sMyChecks));
    sPoll = 0;
    sCheckFrame = sCheck = 0;
    sDesyncReported = 0;
    sLost = 0;
    memset(sDropped, 0, sizeof(sDropped));
    sIdleWarning = 0;
    memset(sRatingChange, 0, sizeof(sRatingChange));
    {
        int i;

        // Online everybody brings the points of their profile. A local
        // wireless session has its own, from 0, and they end with it: two
        // consoles in one room could feed each other points.
        for (i = 0; i < MAX_PLAYERS; i++) {
            sSessionRatings[i] = sOnline ? sListRatings[i] : RATING_START;
        }
    }
    sSlot = slot;
    sIsHost = isHost;
    sPlayers = players;
    sLobby = LOBBY_SESSION;
    {
        extern s32 gRandomTrace;        // src/hasm/math_util.c

        gRandomTrace = sVerbose;
    }
    printf("NETPLAY: %d players, this console is player %d, delay %d, logic rate %d, seed %08lx\n", players, slot + 1,
           sDelay, sRate, (unsigned long) sSeed);
}

static void lobby_receive(void) {
    union { LobbyPacket lobby; InputPacket input; u8 bytes[256]; } p;
    int node, size;

    while ((size = link_receive(&p, sizeof(p), &node)) > 0) {
        if (size < (int) sizeof(LobbyPacket) || p.lobby.magic != MAGIC || p.lobby.protocol != PROTOCOL) {
            continue;
        }
        if (sLobby == LOBBY_HOSTING || (sLobby == LOBBY_STARTING && sIsHost)) {
            if (node < 1 || node >= LINK_MAX_NODES) {
                continue;
            }
            if (p.lobby.type == PKT_HELLO && sLobby == LOBBY_HOSTING) {
                if (!sMembers[node].present) {
                    printf("NETPLAY: %s joined\n", p.lobby.names[0]);
                }
                sMembers[node].present = 1;
                sMembers[node].isNew = p.lobby.isNew;
                sMembers[node].rating = p.lobby.ratings[0];
                sMembers[node].heard = svcGetSystemTick();
                sMembers[node].stamp = p.lobby.stamp;
                sMembers[node].rttMs = p.lobby.rttMs;
                sMembers[node].roster = p.lobby.roster;
                copy_name(sMembers[node].name, p.lobby.names[0]);
                {
                    // Answered at once, so that the joiner's round trip
                    // measures the network and not this lobby's pace.
                    int slotOfNode[LINK_MAX_NODES];

                    host_build_list(slotOfNode);
                    if (slotOfNode[node] >= 0) {
                        send_lobby_packet(node, PKT_LOBBY, slotOfNode[node]);
                    }
                }
            } else if (p.lobby.type == PKT_BYE) {
                sMembers[node].present = 0;
            } else if (p.lobby.type == PKT_READY) {
                sMembers[node].ready = 1;
                sMembers[node].heard = svcGetSystemTick();
            }
        } else if (sLobby == LOBBY_JOINED && node == 0) {
            int i;

            if (p.lobby.type == PKT_LOBBY || p.lobby.type == PKT_START) {
                sHostHeard = svcGetSystemTick();
                if (p.lobby.stamp != 0 && p.lobby.stamp != sLastStampSeen) {
                    // Each of this console's clock values comes back once
                    // promptly; later copies of it are the lobby's upkeep.
                    u32 sample = ((u32) (svcGetSystemTick() / TICKS_PER_MS) | 1) - p.lobby.stamp;

                    sLastStampSeen = p.lobby.stamp;
                    sMyRttMs = (sMyRttMs == 0 || sample < sMyRttMs) ? sample : sMyRttMs + (sample - sMyRttMs) / 4;
                }
                sListCount = p.lobby.count <= MAX_PLAYERS ? p.lobby.count : MAX_PLAYERS;
                sListSlot = p.lobby.slot;
                for (i = 0; i < MAX_PLAYERS; i++) {
                    copy_name(sListNames[i], p.lobby.names[i]);
                    sListRatings[i] = p.lobby.ratings[i];
                }
            }
            if (p.lobby.type == PKT_START && p.lobby.slot < MAX_PLAYERS) {
                sDelay = p.lobby.delay;
                sRate = p.lobby.rate;
                sSeed = p.lobby.seed;
                sSessionRoster = p.lobby.roster;
                for (i = 0; i < 3; i++) {       // the host starts on the first that arrives
                    send_lobby_packet(0, PKT_READY, p.lobby.slot);
                }
                session_begin(p.lobby.count, p.lobby.slot, 0);
                return;
            } else if (p.lobby.type == PKT_BYE) {
                printf("NETPLAY: the host closed the game\n");
                netplay_lobby_search();
                return;
            }
        }
    }
}

void netplay_lobby_tick(void) {
    int slotOfNode[LINK_MAX_NODES];
    int node, upkeep;

    if (sLobby == LOBBY_OFF || sLobby == LOBBY_SESSION) {
        return;
    }
    upkeep = ms_since(sLastUpkeep) >= HELLO_EVERY_MS;
    if (upkeep) {
        sLastUpkeep = svcGetSystemTick();
    }
    lobby_receive();

    switch (sLobby) {
        case LOBBY_HOSTING:
            for (node = 1; node < LINK_MAX_NODES; node++) {
                if (sMembers[node].present && ms_since(sMembers[node].heard) > MEMBER_TIMEOUT_MS) {
                    printf("NETPLAY: %s left\n", sMembers[node].name);
                    sMembers[node].present = 0;
                }
            }
            host_build_list(slotOfNode);
            if (upkeep) {
                GameInfo info;

                make_info(&info, sListCount);
                link_host_set_info((const unsigned char *) &info);
                for (node = 1; node < LINK_MAX_NODES; node++) {
                    if (slotOfNode[node] >= 0) {
                        send_lobby_packet(node, PKT_LOBBY, slotOfNode[node]);
                    }
                }
            }
            break;
        case LOBBY_STARTING: {
            // Each joiner is told its slot until it answers.
            int waiting = 0;

            host_build_list(slotOfNode);
            for (node = 1; node < LINK_MAX_NODES; node++) {
                if (slotOfNode[node] >= 0 && !sMembers[node].ready) {
                    waiting++;
                    if (upkeep) {
                        send_lobby_packet(node, PKT_START, slotOfNode[node]);
                    }
                }
            }
            if (waiting == 0) {
                for (node = 1; node < LINK_MAX_NODES; node++) {
                    if (slotOfNode[node] >= 0) {
                        sSlotNode[slotOfNode[node]] = node;
                    }
                }
                session_begin(sListCount, 0, 1);
            } else if (ms_since(sStartBegan) > START_TIMEOUT_MS) {
                printf("NETPLAY: a player did not answer the start\n");
                for (node = 1; node < LINK_MAX_NODES; node++) {
                    if (!sMembers[node].ready) {
                        sMembers[node].present = 0;
                    }
                    sMembers[node].ready = 0;
                }
                sLobby = LOBBY_HOSTING;
            }
            break;
        }
        case LOBBY_SEARCHING:
            sGameCount = link_search(sGames, LINK_MAX_GAMES);
            break;
        case LOBBY_JOINED:
            if (upkeep) {
                send_lobby_packet(0, PKT_HELLO, 0);
            }
            if (ms_since(sHostHeard) > (sOnline ? MEMBER_TIMEOUT_MS * 3 : MEMBER_TIMEOUT_MS)) {
                printf("NETPLAY: lost the host\n");
                netplay_lobby_search();
            }
            break;
        default:
            break;
    }
}

int netplay_lobby_enter(int online) {
    static int sProfileLoaded;

    if (sLobby != LOBBY_OFF) {
        return 1;
    }
    if (!sProfileLoaded) {
        sProfileLoaded = 1;
        profile_load();
    }
    link_stop();
    sOnline = online;
    sCode[0] = '\0';
    sMyRttMs = 0;
    if (!link_start(sDir, sOnline)) {
        return 0;
    }
    sGameCount = 0;
    sListCount = 0;
    return 1;
}

// Out of any game hosted or joined, the service still running.
static void leave_game(void) {
    int node, i;

    if (sLobby == LOBBY_HOSTING || sLobby == LOBBY_STARTING) {
        for (i = 0; i < 3; i++) {
            for (node = 1; node < LINK_MAX_NODES; node++) {
                if (sMembers[node].present) {
                    send_lobby_packet(node, PKT_BYE, 0);
                }
            }
        }
    } else if (sLobby == LOBBY_JOINED) {
        for (i = 0; i < 3; i++) {
            send_lobby_packet(0, PKT_BYE, 0);
        }
    }
    // The link layer has one way out of a game: stop and start again.
    link_stop();
    link_start(sDir, sOnline);
    sCode[0] = '\0';
    memset(sMembers, 0, sizeof(sMembers));
    sListCount = 0;
    sGameCount = 0;
}

void netplay_lobby_leave(void) {
    if (sLobby == LOBBY_OFF) {
        link_stop();
        return;
    }
    if (sLobby != LOBBY_SESSION) {
        leave_game();
    }
    link_stop();
    sLobby = LOBBY_OFF;
    sPlayers = 0;
}

int netplay_lobby_state(void) {
    return sLobby;
}

int netplay_lobby_host(void) {
    GameInfo info;

    if (sLobby != LOBBY_OFF && sLobby != LOBBY_SEARCHING) {
        leave_game();
    }
    memset(sMembers, 0, sizeof(sMembers));
    sListCount = 1;
    sListSlot = 0;
    copy_name(sListNames[0], sMyName);
    make_info(&info, 1);
    if (!link_host((const unsigned char *) &info)) {
        printf("NETPLAY: could not open a game\n");
        return 0;
    }
    sIsHost = 1;
    sLobby = LOBBY_HOSTING;
    printf("NETPLAY: hosting over %s\n", link_kind());
    if (sOnline) {
        unsigned address = 0;
        int port = 0;

        sCodePublic = link_host_address(&address, &port) == 2;
        code_from_address(address, port, sCode);
        printf("NETPLAY: game code %s (%s)\n", sCode,
               sCodePublic ? "reachable from the internet" : "this network only: the router opened no port");
    }
    return 1;
}

const char *netplay_lobby_code(void) {
    return (sLobby == LOBBY_HOSTING || sLobby == LOBBY_STARTING) ? sCode : "";
}

int netplay_lobby_code_is_public(void) {
    return sCodePublic;
}

int netplay_lobby_join_code(const char *text) {
    unsigned address = 0;
    int port = 0;

    if (!sOnline || !code_to_address(text, &address, &port)) {
        return 0;
    }
    if (sLobby != LOBBY_OFF && sLobby != LOBBY_SEARCHING) {
        leave_game();
    }
    if (!link_join_address(address, port)) {
        printf("NETPLAY: could not join\n");
        return 0;
    }
    sIsHost = 0;
    sListCount = 0;
    sHostHeard = svcGetSystemTick();
    sLobby = LOBBY_JOINED;
    send_lobby_packet(0, PKT_HELLO, 0);
    printf("NETPLAY: joining by code\n");
    return 1;
}

// The code to join, from the player: the console's keyboard, or a line
// "code XXXX-XXXX-XXXX" in online.txt (the emulator tests, which have no
// keyboard to type on). Returns 0 when the player cancelled.
int netplay_ask_code(char *out, int size) {
    char path[160], line[96], word[16], value[32];
    SwkbdState keyboard;
    FILE *f;

    snprintf(path, sizeof(path), "%s/online.txt", sDir);
    f = fopen(path, "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (sscanf(line, "%15s %31s", word, value) == 2 && strcmp(word, "code") == 0) {
                fclose(f);
                snprintf(out, (size_t) size, "%s", value);
                return 1;
            }
        }
        fclose(f);
    }
    swkbdInit(&keyboard, SWKBD_TYPE_QWERTY, 2, 14);
    swkbdSetHintText(&keyboard, "Game code, like ABCD-EFGH-JKLM");
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    out[0] = '\0';
    return swkbdInputText(&keyboard, out, (size_t) size) == SWKBD_BUTTON_CONFIRM && out[0] != '\0';
}

int netplay_lobby_search(void) {
    if (sLobby == LOBBY_HOSTING || sLobby == LOBBY_STARTING || sLobby == LOBBY_JOINED) {
        leave_game();
    }
    sIsHost = 0;
    sGameCount = 0;
    sLobby = LOBBY_SEARCHING;
    return 1;
}

static const GameInfo *game_info(int game) {
    const GameInfo *info;

    if (game < 0 || game >= sGameCount) {
        return NULL;
    }
    info = (const GameInfo *) sGames[game].info;
    return (info->magic == MAGIC && info->protocol == PROTOCOL) ? info : NULL;
}

int netplay_lobby_game_count(void) {
    return sLobby == LOBBY_SEARCHING ? sGameCount : 0;
}

const char *netplay_lobby_game_name(int game) {
    const GameInfo *info = game_info(game);

    return info != NULL ? info->name : "?";
}

int netplay_lobby_game_players(int game) {
    const GameInfo *info = game_info(game);

    return info != NULL ? info->players : 0;
}

int netplay_lobby_join(int game) {
    if (sLobby != LOBBY_SEARCHING || game_info(game) == NULL || game_info(game)->players >= MAX_PLAYERS) {
        return 0;
    }
    if (!link_join(game)) {
        printf("NETPLAY: could not join\n");
        return 0;
    }
    sListCount = 0;
    sHostHeard = svcGetSystemTick();
    sLobby = LOBBY_JOINED;
    send_lobby_packet(0, PKT_HELLO, 0);
    printf("NETPLAY: joined %s\n", netplay_lobby_game_name(game));
    return 1;
}

int netplay_lobby_player_count(void) {
    return sListCount;
}

const char *netplay_lobby_player_name(int slot) {
    return (slot >= 0 && slot < sListCount) ? sListNames[slot] : "";
}

const char *netplay_player_name(int player) {
    return (sPlayers != 0 && player >= 0 && player < sPlayers) ? sListNames[player] : "";
}

int netplay_lobby_start(void) {
    int node, allNew = sIsNew;

    if (sLobby != LOBBY_HOSTING || sListCount < 2) {
        return 0;
    }
    // Added characters are played with only if every console has the same set.
    sSessionRoster = modchar_roster();
    for (node = 1; node < LINK_MAX_NODES; node++) {
        sMembers[node].ready = 0;
        if (sMembers[node].present && !sMembers[node].isNew) {
            allNew = 0;
        }
        if (sMembers[node].present && sMembers[node].roster != sSessionRoster) {
            sSessionRoster = 0;
        }
    }
    // The pace every console can keep: 60 Hz between New 3DS consoles, 30 Hz
    // when an Old 3DS is playing.
    sRate = allNew ? 1 : 2;
    // A sample has to reach every console before it is used. The longest
    // way is from one joiner through the host to another: half of each one's
    // round trip, so no more than the slowest joiner's whole round trip.
    {
        u32 worstMs = 0, pollMs = (u32) (sRate * 1000 / 60);

        for (node = 1; node < LINK_MAX_NODES; node++) {
            if (sMembers[node].present && sMembers[node].rttMs > worstMs) {
                worstMs = sMembers[node].rttMs;
            }
        }
        sDelay = (int) ((worstMs + 20) / pollMs) + 1;
        if (sDelay < DELAY_POLLS) {
            sDelay = DELAY_POLLS;
        }
        if (sDelay > MAX_DELAY_POLLS) {
            sDelay = MAX_DELAY_POLLS;
        }
        printf("NETPLAY: slowest round trip %lu ms, samples used %d polls late\n", (unsigned long) worstMs, sDelay);
    }
    sSeed = (u32) svcGetSystemTick() | 1;
    link_host_close_doors();
    sStartBegan = svcGetSystemTick();
    sLobby = LOBBY_STARTING;
    return 1;
}

// ---- the session

static void store_input(int player, u32 poll, NetInput in) {
    if (player < 0 || player >= MAX_PLAYERS) {
        return;
    }
    if (sHave[player][poll % WINDOW] == poll + 1) {
        return;
    }
    sInputs[player][poll % WINDOW] = in;
    sHave[player][poll % WINDOW] = poll + 1;
    if (poll + 1 > sLatest[player]) {
        sLatest[player] = poll + 1;
    }
}

static int has_input(int player, u32 poll) {
    return sHave[player][poll % WINDOW] == poll + 1;
}

// The newest samples known for `player`, to one node.
static void send_inputs(int node, int player) {
    InputPacket p;
    u32 last = sLatest[player], first;
    int i;

    if (last == 0) {
        return;
    }
    first = last > REDUNDANCY ? last - REDUNDANCY : 0;
    memset(&p, 0, sizeof(p));
    p.magic = MAGIC;
    p.type = PKT_INPUT;
    p.player = (u8) player;
    p.frame = first;
    for (i = 0; first + i < last && i < REDUNDANCY; i++) {
        if (!has_input(player, first + i)) {
            break;
        }
        p.in[i] = sInputs[player][(first + i) % WINDOW];
    }
    p.count = (u8) i;
    p.checkFrame = sCheckFrame;
    p.check = sCheck;
    link_send(node, &p, sizeof(p));
}

static void send_all(void) {
    int peer, player;

    if (sIsHost) {
        // Everybody's samples to every joiner, except its own.
        for (peer = 1; peer < sPlayers; peer++) {
            if (sDropped[peer]) {
                continue;
            }
            for (player = 0; player < sPlayers; player++) {
                if (player != peer) {
                    send_inputs(sSlotNode[peer], player);
                }
            }
        }
    } else {
        send_inputs(0, sSlot);
    }
}

static void session_lost(const char *why) {
    printf("NETPLAY: session over at poll %lu: %s\n", (unsigned long) sPoll, why);
    sPlayers = 0;
    sLost = 1;
}

static void receive_all(void) {
    union { LobbyPacket lobby; InputPacket input; u8 bytes[256]; } p;
    int node, size, i;

    while (sPlayers != 0 && (size = link_receive(&p, sizeof(p), &node)) > 0) {
        if (size < 8 || p.input.magic != MAGIC) {
            continue;
        }
        if (p.input.type == PKT_BYE) {
            session_lost("another player left");
            return;
        }
        if (p.input.type == PKT_START && !sIsHost && size >= (int) sizeof(LobbyPacket)) {
            send_lobby_packet(0, PKT_READY, sSlot);     // the host missed the first answer
            continue;
        }
        if (p.input.type != PKT_INPUT || size < (int) sizeof(InputPacket) || p.input.player >= MAX_PLAYERS ||
            p.input.count > REDUNDANCY) {
            continue;
        }
        for (i = 0; i < p.input.count; i++) {
            store_input(p.input.player, p.input.frame + i, p.input.in[i]);
        }
        // The sender's state check for a poll this console has also passed.
        // A joiner hears the host's own check with the host's own samples.
        if ((sIsHost || p.input.player == 0) && p.input.checkFrame != 0 && p.input.checkFrame + WINDOW > sPoll &&
            p.input.checkFrame < sPoll && sMyChecks[p.input.checkFrame % WINDOW] != p.input.check &&
            !sDesyncReported) {
            sDesyncReported = 1;
            printf("NETPLAY: OUT OF STEP at poll %lu: here %08lx, player %d %08lx\n",
                   (unsigned long) p.input.checkFrame, (unsigned long) sMyChecks[p.input.checkFrame % WINDOW],
                   p.input.player + 1, (unsigned long) p.input.check);
        }
    }
}

int netplay_players(void) {
    return sPlayers;
}

int netplay_slot(void) {
    return sSlot;
}

unsigned netplay_session_roster(void) {
    return sSessionRoster;
}

int netplay_fixed_rate(void) {
    return sPlayers != 0 ? sRate : 0;
}

unsigned netplay_seed(void) {
    return sSeed;
}

unsigned netplay_poll_count(void) {
    return sPoll;
}

int netplay_take_lost(void) {
    int lost = sLost;

    sLost = 0;
    return lost;
}

// ---- players the game takes out
//
// The game sees every player's pad on every console at the same poll, so it
// can decide on every console at once that a player has been away too long
// (pc_session_watch_idle in src/objects.c). It warns first, then drops: the
// dropped player's console leaves the session, and the others stop waiting
// for it. The host cannot be dropped and the session kept: everybody's
// samples go through it.

// ---- points
//
// After a race every pair of players settles up, as in Mario Kart Wii: the
// one who came in ahead wins points and the other loses some. How many is
// decided twice over.
//
//   1. By the two players' difference (the stake): 16 between equals, more
//      when the winner had fewer points than the loser, less when the winner
//      had more, from 2 to 48. Beating somebody 1000 points above is worth
//      47; beating somebody 450 or more below is worth 2.
//   2. By each player's own points. The winner is paid the whole stake up to
//      0 points and less of it the higher they stand, half of it from 4000.
//      The loser pays half the stake at 0 points, a quarter at -1000 and
//      below, and all of it from 2000.
//
// So a player low down loses little and gains much, a player high up gains
// little and loses much, and nobody falls far below zero. Two new players:
// the winner +16, the loser -8. Four new players: +48, +24, 0, -24.
//
// Whole numbers only, from values every console has, so every console
// arrives at the same points for everybody. `places` is each player's
// finishing position among all racers (1 = first, 0 = did not take part);
// only the order between players matters, computer racers pay and take
// nothing. Online the points are the profile's and are saved; in a local
// wireless session they start at 0 and end with it.

#define POINTS_STAKE_EVEN 16    // between two players with the same points
#define POINTS_STAKE_MIN 2
#define POINTS_STAKE_MAX 48
#define POINTS_PER_STAKE 32     // points of difference that move the stake by one

static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

static int points_stake(int winner, int loser) {
    return clamp_int(POINTS_STAKE_EVEN + (loser - winner) / POINTS_PER_STAKE, POINTS_STAKE_MIN, POINTS_STAKE_MAX);
}

// What the winner of a stake gets: all of it at 0 points or below, half from 4000.
static int points_won(int stake, int own) {
    int won = stake * (8000 - clamp_int(own, 0, 4000)) / 8000;

    return won < 1 ? 1 : won;
}

// What its loser pays: a quarter at -1000 or below, half at 0, all from 2000.
static int points_lost(int stake, int own) {
    int lost = stake * clamp_int(50 + own / 40, 25, 100) / 100;

    return lost < 1 ? 1 : lost;
}

int netplay_session_online(void) {
    return sPlayers != 0 && sOnline;
}

int netplay_rating(int player) {
    if (sPlayers == 0) {
        return sMyRating;
    }
    return (player >= 0 && player < MAX_PLAYERS) ? sSessionRatings[player] : 0;
}

int netplay_rating_change(int player) {
    return (sPlayers != 0 && player >= 0 && player < MAX_PLAYERS) ? sRatingChange[player] : 0;
}

int netplay_my_rating(void) {
    return sMyRating;
}

void netplay_report_race(const int places[NETPLAY_MAX_PLAYERS]) {
    int change[MAX_PLAYERS] = { 0 };
    int a, b;

    memset(sRatingChange, 0, sizeof(sRatingChange));
    if (sPlayers == 0) {
        return;
    }
    for (a = 0; a < sPlayers; a++) {
        for (b = a + 1; b < sPlayers; b++) {
            int winner, loser, stake;

            if (places[a] <= 0 || places[b] <= 0 || places[a] == places[b]) {
                continue;
            }
            winner = places[a] < places[b] ? a : b;
            loser = winner == a ? b : a;
            stake = points_stake(sSessionRatings[winner], sSessionRatings[loser]);
            change[winner] += points_won(stake, sSessionRatings[winner]);
            change[loser] -= points_lost(stake, sSessionRatings[loser]);
        }
    }
    for (a = 0; a < sPlayers; a++) {
        int rating = sSessionRatings[a] + change[a];

        rating = rating < RATING_MIN ? RATING_MIN : (rating > RATING_MAX ? RATING_MAX : rating);
        sRatingChange[a] = rating - sSessionRatings[a];
        sSessionRatings[a] = rating;
    }
    for (a = 0; a < sPlayers; a++) {
        printf("NETPLAY: player %d place %d points %d (%+d)\n", a + 1, places[a], sSessionRatings[a], sRatingChange[a]);
    }
    if (!sOnline) {
        return;
    }
    sMyRating = sSessionRatings[sSlot];
    sMyRaces++;
    profile_save();
    printf("NETPLAY: points %d (%+d) after %d online races\n", sMyRating, sRatingChange[sSlot], sMyRaces);
}

int netplay_lobby_player_rating(int slot) {
    return (slot >= 0 && slot < sListCount) ? sListRatings[slot] : 0;
}

void netplay_set_idle_warning(int player) {
    sIdleWarning = sPlayers != 0 ? player : 0;
}

int netplay_idle_warning(void) {
    return sPlayers != 0 ? sIdleWarning : 0;
}

int netplay_player_dropped(int player) {
    return sPlayers != 0 && player >= 0 && player < MAX_PLAYERS && sDropped[player];
}

void netplay_drop_player(int player) {
    if (sPlayers == 0 || player < 0 || player >= sPlayers || sDropped[player]) {
        return;
    }
    printf("NETPLAY: player %d dropped for being idle at poll %lu\n", player + 1, (unsigned long) sPoll);
    sDropped[player] = 1;
    sIdleWarning = 0;
    if (player == sSlot) {
        if (sOnline) {
            // Leaving a race this way counts as losing it to everybody: the
            // others' consoles rank the dropped player last when it ends.
            sMyRating -= points_lost(POINTS_STAKE_EVEN, sMyRating) * (sPlayers - 1);
            if (sMyRating < RATING_MIN) {
                sMyRating = RATING_MIN;
            }
            sMyRaces++;
            profile_save();
        }
        session_lost("this console's player was idle for a minute");
    } else if (player == 0) {
        session_lost("the host was idle for a minute");
    }
}

void netplay_back_to_lobby(void) {
    int slot;

    if (sPlayers == 0) {
        return;     // the session was lost already; the game goes to the title screen
    }
    printf("NETPLAY: back to the lobby at poll %lu\n", (unsigned long) sPoll);
    if (sIsHost) {
        // The players of the session are the lobby's members again, except
        // those the session dropped; everyone counts as just heard from.
        for (slot = 1; slot < sPlayers; slot++) {
            int node = sSlotNode[slot];

            if (node >= 1 && node < LINK_MAX_NODES) {
                sMembers[node].present = !sDropped[slot];
                sMembers[node].ready = 0;
                sMembers[node].heard = svcGetSystemTick();
            }
        }
        link_host_open_doors();
        sLobby = LOBBY_HOSTING;
    } else {
        sHostHeard = svcGetSystemTick();
        sLobby = LOBBY_JOINED;
    }
    sPlayers = 0;
}

void netplay_end(void) {
    int node, i;

    if (sPlayers == 0) {
        // Already over (the others left, or this player was dropped): only
        // the service is still up.
        if (sLobby == LOBBY_SESSION) {
            link_stop();
            sLobby = LOBBY_OFF;
        }
        return;
    }
    for (i = 0; i < 3; i++) {
        if (sIsHost) {
            LobbyPacket p;

            memset(&p, 0, sizeof(p));
            p.magic = MAGIC;
            p.type = PKT_BYE;
            p.protocol = PROTOCOL;
            for (node = 1; node < sPlayers; node++) {
                link_send(sSlotNode[node], &p, sizeof(p));
            }
        } else {
            send_lobby_packet(0, PKT_BYE, sSlot);
        }
    }
    printf("NETPLAY: session ended at poll %lu\n", (unsigned long) sPoll);
    sPlayers = 0;
    link_stop();
    sLobby = LOBBY_OFF;
}

// One controller poll: takes this console's sample, returns everybody's for
// this poll number. Waits for the others.
void netplay_poll(unsigned short buttons, signed char x, signed char y, unsigned short *outButtons,
                  signed char *outX, signed char *outY) {
    NetInput local = { buttons, x, y };
    u64 start = svcGetSystemTick(), lastSend = 0;
    int player;

    // A sample read now is the one used DELAY polls from now; the first
    // DELAY polls of a session are neutral on every console.
    if (sPoll == 0) {
        NetInput neutral = { 0, 0, 0 };
        u32 f;

        for (f = 0; f < (u32) sDelay; f++) {
            for (player = 0; player < sPlayers; player++) {
                store_input(player, f, neutral);
            }
        }
    }
    store_input(sSlot, sPoll + sDelay, local);

    sMyChecks[sPoll % WINDOW] = pc_session_state_check();
    if ((sPoll & 15) == 0) {
        sCheckFrame = sPoll;
        sCheck = sMyChecks[sPoll % WINDOW];
    }

    for (;;) {
        u64 now = svcGetSystemTick();
        int complete = 1;

        if (lastSend == 0 || now - lastSend > SYSCLOCK_ARM11 / 100) {
            send_all();
            lastSend = now;
        }
        receive_all();
        if (sPlayers == 0) {
            break;      // the session ended while waiting
        }
        for (player = 0; player < sPlayers; player++) {
            if (!sDropped[player] && !has_input(player, sPoll)) {
                complete = 0;
            }
        }
        if (complete) {
            break;
        }
        if ((now - start) / TICKS_PER_MS > SESSION_TIMEOUT_MS) {
            session_lost("no word from the other players");
            break;
        }
        if (!aptMainLoop()) {
            exit(0);
        }
        svcSleepThread(500000);
    }
    for (player = 0; player < MAX_PLAYERS; player++) {
        const NetInput *in = &sInputs[player][sPoll % WINDOW];
        int live = player < sPlayers && sPlayers != 0 && !sDropped[player];

        outButtons[player] = live ? in->buttons : 0;
        outX[player] = live ? in->x : 0;
        outY[player] = live ? in->y : 0;
    }
    if (sVerbose || (sPoll & 255) == 0) {
        printf("NETPLAY: poll %lu check %08lx\n", (unsigned long) sPoll, (unsigned long) sMyChecks[sPoll % WINDOW]);
    }
    if (sVerbose) {
        extern u32 gCheckParts[3];      // localplay_menu.inc: the random seed, the racers' positions, the menu

        printf("NETPARTS: poll %lu seed %08lx positions %08lx menu %08lx\n", (unsigned long) sPoll,
               (unsigned long) gCheckParts[0], (unsigned long) gCheckParts[1], (unsigned long) gCheckParts[2]);
    }
    sPoll++;
}

void netplay_init(const char *dir) {
    char path[160];
    FILE *f;

    snprintf(sDir, sizeof(sDir), "%s", dir);
    APT_CheckNew3DS(&sIsNew);
    read_my_name();
    // NETLOG.TXT in the game's folder: log every poll's state check, to find
    // where two consoles part ways.
    snprintf(path, sizeof(path), "%s/NETLOG.TXT", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        extern u32 gTraceFrom, gTraceTo;        // localplay_menu.inc
        unsigned from = 0, to = 0;

        if (fscanf(f, "%u %u", &from, &to) == 2) {
            gTraceFrom = from;
            gTraceTo = to;
        }
        fclose(f);
        sVerbose = 1;
    }
}
