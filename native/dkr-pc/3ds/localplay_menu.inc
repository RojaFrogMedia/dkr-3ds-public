// The Multiplayer screen: part of src/menu.c (included at its end on the 3DS)
// so that it is drawn and worked exactly like the game's own menus, with the
// game's fonts, sounds and transitions.
//
// It is the lobby of a session between consoles (3ds/netplay.c), in the way
// of Mario Kart DS:
//
//   MULTIPLAYER       LOCAL WIRELESS / ONLINE
//   either            HOST A GAME / JOIN A GAME
//   host              the list of players as they join; A starts with two or
//                     more. Online, the game's code is shown: the others
//                     type it in to join.
//   join, local       the games nearby; A joins one
//   join, online      the console's keyboard asks for the code
//   joined            the same list of players, waiting for the host
//
// When the host starts, every console leaves this screen for the character
// select at the same controller poll, with the session's random seed, the
// session's save data and every player already in: from there on the game's
// own multiplayer menus run on all consoles in step, each player with their
// own console as controller and, in a race, their own view on the whole
// screen instead of a part of a split one.

#include "../3ds/netplay.h"

extern void pc_gfx_set_local_view(s32 player);
extern void pc_eeprom_session(s32 on);
extern s32 gSaveDataFlags;

enum { LP_MODE, LP_CHOICE, LP_HOST, LP_SEARCH, LP_JOINED, LP_NO_SERVICE, LP_NO_GAME, LP_STARTING };

static s32 sLocalOnline;            // the lobby is an online one

static s32 sLocalStage;
static s32 sLocalCursor;
static s32 sLocalStartFrames;       // frames since the session began
static s32 sLocalSavedCodes;        // the player's own cheats, put back after the session
static s32 sLocalSessionOpen;       // the game is set up for a session
static s32 sLocalChooser;           // the player who works the track and vehicle menus

#define LP_START_FRAMES 30          // from the host's start to the character select
#define LP_ROW_Y 78
#define LP_ROW_STEP 26

/**
 * What every console must have the same before the first shared frame:
 * which characters and tracks exist (the save data and the cheats).
 */
static void localplay_session_open(void) {
    if (sLocalSessionOpen) {
        return;
    }
    sLocalSessionOpen = TRUE;
    sLocalChooser = PLAYER_ONE;
    pc_gfx_set_local_view(netplay_slot());
    pc_eeprom_session(TRUE);
    gSaveDataFlags = SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES |
                     SAVE_DATA_FLAG_READ_SAVE_DATA | SAVE_DATA_FLAG_READ_EEPROM_SETTINGS;
    bzero(gLocalPlacements, sizeof(gLocalPlacements));
    sLocalSavedCodes = gActiveMagicCodes;
    // Everybody may pick anybody: both secret characters, and the same
    // character as another player (nobody sees the others choose).
    gActiveMagicCodes = CHEAT_CONTROL_TT | CHEAT_CONTROL_DRUMSTICK | CHEAT_SELECT_SAME_PLAYER;
}

/**
 * The session is over (everybody is back at the title screen, or the others
 * went away): the console's own save and settings again.
 */
void localplay_session_close(void) {
    if (!sLocalSessionOpen) {
        return;
    }
    sLocalSessionOpen = FALSE;
    netplay_end();
    pc_gfx_set_local_view(-1);
    pc_eeprom_session(FALSE);
    gSaveDataFlags = SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES |
                     SAVE_DATA_FLAG_READ_SAVE_DATA | SAVE_DATA_FLAG_READ_EEPROM_SETTINGS;
    gActiveMagicCodes = sLocalSavedCodes;
}

/**
 * The session was lost while one of its menus was up (src/thread3_main.c):
 * gives back what that menu holds, as the menu does itself when it is left.
 * The game goes to the title screen from here.
 */
void localplay_menu_abandon(void) {
    switch (gCurrentMenuId) {
        case MENU_CHARACTER_SELECT:
            charselect_free();
            break;
        case MENU_RESULTS:
            results_free();
            break;
        case MENU_LOCAL_PLAY:       // the half second between the host's start and the character select
        case MENU_SESSION_TRACKS:
            unload_font(ASSET_FONTS_BIGFONT);
            break;
        default:
            break;
    }
    music_change_on();
}

/**
 * The session stops for the lobby to take more players: as
 * localplay_session_close, but the connection stays (netplay_back_to_lobby).
 */
static void localplay_session_to_lobby(void) {
    if (!sLocalSessionOpen) {
        return;
    }
    sLocalSessionOpen = FALSE;
    netplay_back_to_lobby();
    pc_gfx_set_local_view(-1);
    pc_eeprom_session(FALSE);
    gSaveDataFlags = SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES |
                     SAVE_DATA_FLAG_READ_SAVE_DATA | SAVE_DATA_FLAG_READ_EEPROM_SETTINGS;
    gActiveMagicCodes = sLocalSavedCodes;
}

/**
 * A number that is the same on every console as long as they are in step:
 * the random seed, where the menus are, and where the racers are.
 */
// The check's parts, for NETLOG.TXT: when two consoles part ways, which part
// differs first says where to look.
u32 gCheckParts[3];
extern s32 gRandomTrace;
// NETLOG.TXT may hold two poll numbers: between them every object is logged
// every poll instead of every sixteenth (3ds/netplay.c reads them).
u32 gTraceFrom, gTraceTo;

u32 pc_session_state_check(void) {
    u32 check = (u32) get_rng_seed();
    Object **racers;
    s32 numRacers = 0;
    s32 i;

    // Until the consoles reach the character select together they are each
    // still in their own lobby screen.
    if (gCurrentMenuId == MENU_LOCAL_PLAY) {
        return 0;
    }

    gCheckParts[0] = check;      // the random seed
    gCheckParts[1] = 0;          // the racers' positions, summed below
    gCheckParts[2] = ((u32) gCurrentMenuId << 16) | ((u32) gMenuDelay & 0xFFFF);
    check = (check * 31) + (u32) gCurrentMenuId;
    check = (check * 31) + (u32) gMenuDelay;
    if (get_game_mode() != GAMEMODE_MENU && gRandomTrace) {
        // NETLOG.TXT: every object of the level, one line each poll for the
        // lot and, every 16th poll, one per object: when something that is
        // not a racer differs between two consoles, this says what.
        extern Object **gObjPtrList;
        extern s32 gObjectCount;
        u32 all = 0;
        s32 real = 0;

        for (i = 0; i < gObjectCount; i++) {
            Object *obj = gObjPtrList[i];
            u32 one;

            // Particles are objects too, and differ between consoles by design.
            if (obj == NULL || (obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
                continue;
            }
            real++;
            one = *(u32 *) &obj->trans.x_position * 31 + *(u32 *) &obj->trans.y_position;
            one = one * 31 + *(u32 *) &obj->trans.z_position;
            one = one * 31 + (u32) obj->behaviorId;
            all = all * 31 + one;
            if ((netplay_poll_count() & 15) == 0 ||
                (netplay_poll_count() >= gTraceFrom && netplay_poll_count() < gTraceTo)) {
                stubbed_printf("NETOBJ %d %d %d %x\n", netplay_poll_count(), real, obj->behaviorId, one);
            }
        }
        stubbed_printf("NETOBJS %d %d %x\n", netplay_poll_count(), real, all);
    }
    if (get_game_mode() != GAMEMODE_MENU) {
        racers = get_racer_objects(&numRacers);
        for (i = 0; i < numRacers && racers != NULL; i++) {
            if (racers[i] != NULL) {
                check = (check * 31) + *(u32 *) &racers[i]->trans.x_position;
                check = (check * 31) + *(u32 *) &racers[i]->trans.y_position;
                check = (check * 31) + *(u32 *) &racers[i]->trans.z_position;
                gCheckParts[1] = (gCheckParts[1] * 31) + *(u32 *) &racers[i]->trans.x_position +
                                 *(u32 *) &racers[i]->trans.z_position;
                if (gRandomTrace) {
                    // NETLOG.TXT: every racer's position and a sum over each
                    // 32 bytes of its racer data, to see whose differs between
                    // two consoles and whereabouts (include/structs.h has the
                    // offsets of Object_Racer's fields).
                    u8 *bytes = (u8 *) racers[i]->racer;
                    char sums[20 * 9 + 1];
                    s32 chunk, k, n = 0;

                    for (chunk = 0; chunk * 32 < (s32) sizeof(Object_Racer) && chunk < 20; chunk++) {
                        u32 sum = 0;

                        for (k = 0; k < 32 && chunk * 32 + k < (s32) sizeof(Object_Racer); k++) {
                            sum = sum * 31 + bytes[chunk * 32 + k];
                        }
                        n += sprintf(sums + n, " %x", sum);
                    }
                    stubbed_printf("NETRACER %d %d %x %x %x |%s\n", netplay_poll_count(), i,
                                   *(u32 *) &racers[i]->trans.x_position, *(u32 *) &racers[i]->trans.y_position,
                                   *(u32 *) &racers[i]->trans.z_position, sums);
                }
            }
        }
    }
    return check;
}

/**
 * Whether this console shows and hears what `player` does at the character
 * select. In a session each player chooses in private: the consoles all run
 * the same menu, and each draws only its own player's sign and plays only
 * its own player's sounds. Nothing the game decides may depend on this.
 */
s32 localplay_shows_player(s32 player) {
    return netplay_players() == 0 || player == netplay_slot();
}

/**
 * Whether the vehicle for a race is the leader's choice alone: in a session
 * player 1 (the host) picks car, hovercraft or plane for everybody.
 */
s32 localplay_leader_chooses(void) {
    return netplay_players() != 0;
}

/**
 * The player whose pad works the menus between races: the host to begin
 * with, then the winner of each race (the best placed player still in the
 * session), who so gets to pick the next track.
 */
s32 localplay_chooser(void) {
    return sLocalChooser;
}

// One of two lines, for the chooser's own console and for the others'
// ("PLAYER %d ..." is filled in with the chooser's number).
char *localplay_chooser_text(char *whenItIsYou, char *whenItIsNot) {
    static char text[40];

    if (localplay_shows_player(sLocalChooser)) {
        return whenItIsYou;
    }
    sprintf(text, whenItIsNot, sLocalChooser + 1);
    return text;
}

/**
 * A race of a session has ended and the rankings are coming up: on every
 * console at the same poll. The winner becomes the chooser, and the players'
 * points move.
 */
void localplay_race_over(void) {
    int places[NETPLAY_MAX_PLAYERS];
    s32 best = -1;
    s32 i;

    for (i = 0; i < NETPLAY_MAX_PLAYERS; i++) {
        places[i] = (i < netplay_players() && gLocalLastPlace[i] >= 1) ? gLocalLastPlace[i] : 0;
        if (places[i] != 0 && !netplay_player_dropped(i) && (best < 0 || places[i] < places[best])) {
            best = i;
        }
    }
    sLocalChooser = best >= 0 ? best : PLAYER_ONE;
    netplay_report_race(places);
}

// Under the rankings: each player's points and what the race did to them
// (netplay_report_race), and who picks the next track.
static void localplay_results_text(void) {
    char text[16];
    s32 players = netplay_players();
    s32 x = 160 - ((players - 1) << 5);
    s32 change;
    s32 i;

    set_text_font(ASSET_FONTS_FUNFONT);
    set_text_background_colour(0, 0, 0, 0);
    {
        for (i = 0; i < players; i++, x += 64) {
            change = netplay_rating_change(i);
            set_text_colour(0, 0, 0, 255, 255);
            sprintf(text, "%d", netplay_rating(i));
            draw_text(&sMenuCurrDisplayList, x + 1, 181, text, ALIGN_MIDDLE_CENTER);
            set_text_colour(255, 255, 255, 0, 255);
            draw_text(&sMenuCurrDisplayList, x, 180, text, ALIGN_MIDDLE_CENTER);
            if (change >= 0) {
                sprintf(text, "+%d", change);
                set_text_colour(120, 255, 120, 0, 255);
            } else {
                sprintf(text, "-%d", -change);
                set_text_colour(255, 120, 120, 0, 255);
            }
            draw_text(&sMenuCurrDisplayList, x, 194, text, ALIGN_MIDDLE_CENTER);
        }
    }
    set_text_colour(255, 255, 255, 0, 255);
    draw_text(&sMenuCurrDisplayList, POS_CENTRED, 214,
              localplay_chooser_text("YOU CHOOSE THE NEXT TRACK", "PLAYER %d CHOOSES THE NEXT TRACK"),
              ALIGN_MIDDLE_CENTER);
}

/**
 * The player number (1 to 4) to show with racer `racer` of the settings'
 * list in a session, or 0: a computer racer, or no session.
 */
s32 localplay_player_tag(s32 racer) {
    if (netplay_players() == 0 || racer < 0 || racer >= netplay_players()) {
        return 0;
    }
    return racer + 1;
}

// "P2", centred at x, in the small font with a shadow.
static void localplay_draw_tag(s32 x, s32 y, s32 tag) {
    char text[4];

    text[0] = 'P';
    text[1] = (char) ('0' + tag);
    text[2] = '\0';
    set_text_font(ASSET_FONTS_FUNFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 255);
    draw_text(&sMenuCurrDisplayList, x + 1, y + 1, text, ALIGN_MIDDLE_CENTER);
    set_text_colour(255, 240, 80, 0, 255);
    draw_text(&sMenuCurrDisplayList, x, y, text, ALIGN_MIDDLE_CENTER);
}

// The screens themselves (the lobby before a session, the track and vehicle
// choice in one) are in multiplayer_menu.inc, which follows this file.
