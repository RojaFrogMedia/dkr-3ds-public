// The multiplayer screens: text in the game's fonts over the sky background.
//
//   MULTIPLAYER > LOCAL WIRELESS / ONLINE > HOST / JOIN > PLAYERS (the lobby)
//   and, in a session, CHOOSE A TRACK > CHOOSE A VEHICLE > the race.
//
// Included at the end of src/menu.c, after localplay_menu.inc (which holds
// what a session is: who chooses, the state check, the results). Nothing of
// the game's own track select is used in a session any more.
//
// ---------------------------------------------------------------------------
// TO CHANGE THINGS BY HAND, edit the tables in the first part of this file:
//
//   sMpText     every line of words, and where on the screen it is
//   sMpTracks   the tracks that can be chosen, in the order shown
//   sMpVehicles the vehicles' names
//   MP_*        colours, spacing, how fast the cursor pulses
//
// The screen is 320 wide and 240 high; y counts down from the top. Text is
// centred. The rest of the file is how the screens behave and should not
// need touching for wording or layout.
// ---------------------------------------------------------------------------

// ---- layout and colours

#define MP_HEADING_Y 32             // the big heading of every screen
#define MP_ROW_Y 78                 // first row of a list (players, games found, tracks)
#define MP_ROW_STEP 26              // from one row to the next
#define MP_TRACK_ROW_Y 86           // first track of the track list
#define MP_TRACK_ROW_STEP 20
#define MP_UNCHOSEN 150             // how solid the lines of a list are that the cursor is not on
#define MP_DIM 160                  // how solid the small print at the bottom is (0-255)
#define MP_EMPTY_SEAT 96            // and an empty seat in the player list
#define MP_BACKGROUND ASSET_LEVEL_OPTIONSBACKGROUND     // the sky

// ---- the words
//
// One entry per line of text: { y, "TEXT" }. %s and %d are filled in by the
// program (a name, a number) and must stay, in the same order.
enum {
    T_MULTIPLAYER, T_LOCAL, T_ONLINE, T_BACK,
    T_HOST, T_JOIN,
    T_PLAYERS, T_CODE, T_PORT_SHUT, T_START, T_WAIT_PLAYERS, T_BACK_LOW,
    T_PLAYER_ROW, T_PLAYER_ROW_RATED, T_EMPTY_ROW,
    T_SEARCHING, T_GAME_ROW, T_JOIN_OR_BACK,
    T_CONNECTING, T_WAIT_HOST,
    T_NO_INTERNET_1, T_NO_INTERNET_2, T_NO_WIRELESS_1, T_NO_WIRELESS_2, T_NO_GAME_1, T_NO_GAME_2,
    T_GET_READY,
    T_CHOOSE_TRACK, T_WORLD, T_RANDOM, T_MORE_PLAYERS, T_TRACK_HELP, T_TRACK_WATCH,
    T_CHOOSE_VEHICLE, T_VEHICLE_HELP, T_VEHICLE_WATCH,
    T_COUNT
};

typedef struct MpLine {
    s16 y;
    char *text;
} MpLine;

static MpLine sMpText[T_COUNT] = {
    // MULTIPLAYER
    [T_MULTIPLAYER] = { MP_HEADING_Y, "MULTIPLAYER" },
    [T_LOCAL] = { 104, "LOCAL WIRELESS" },
    [T_ONLINE] = { 132, "ONLINE" },
    [T_BACK] = { 206, "B: BACK" },
    // LOCAL WIRELESS or ONLINE
    [T_HOST] = { 104, "HOST A GAME" },
    [T_JOIN] = { 132, "JOIN A GAME" },
    // The lobby
    [T_PLAYERS] = { MP_HEADING_Y, "PLAYERS" },
    [T_CODE] = { 60, "CODE  %s" },                              // online: what the others type in
    [T_PORT_SHUT] = { 170, "HOME NETWORK ONLY: ROUTER PORT IS SHUT" },
    [T_START] = { 190, "A: START" },
    [T_WAIT_PLAYERS] = { 190, "WAITING FOR PLAYERS" },
    [T_BACK_LOW] = { 212, "B: BACK" },
    [T_PLAYER_ROW] = { 0, "%d  %s" },                           // number, name
    [T_PLAYER_ROW_RATED] = { 0, "%d  %s  %d" },                 // number, name, online rating
    [T_EMPTY_ROW] = { 0, "%d  - - -" },
    // Looking for a game
    [T_SEARCHING] = { 118, "SEARCHING%s" },                     // %s: the walking dots
    [T_GAME_ROW] = { 0, "%s  %d OF %d" },                       // host's name, players, seats
    [T_JOIN_OR_BACK] = { 212, "A: JOIN   B: BACK" },
    [T_CONNECTING] = { 118, "CONNECTING%s" },
    [T_WAIT_HOST] = { 190, "WAITING FOR THE HOST" },
    // When it cannot be done
    [T_NO_INTERNET_1] = { 110, "NO INTERNET CONNECTION" },
    [T_NO_INTERNET_2] = { 136, "CHECK THE CONSOLE'S SETTINGS" },
    [T_NO_WIRELESS_1] = { 110, "WIRELESS IS NOT AVAILABLE" },
    [T_NO_WIRELESS_2] = { 136, "SWITCH IT ON AND TRY AGAIN" },
    [T_NO_GAME_1] = { 110, "NO GAME ANSWERED TO THAT CODE" },
    [T_NO_GAME_2] = { 136, "CHECK IT WITH THE HOST" },
    [T_GET_READY] = { 120, "GET READY" },
    // In a session: the track
    [T_CHOOSE_TRACK] = { MP_HEADING_Y, "CHOOSE A TRACK" },
    [T_WORLD] = { 62, "%s" },                                   // the world whose tracks are listed
    [T_RANDOM] = { 176, "RANDOM" },
    [T_MORE_PLAYERS] = { 196, "LET MORE PLAYERS JOIN" },        // back to the lobby
    [T_TRACK_HELP] = { 220, "LEFT RIGHT: WORLD   A: OK   B: CHARACTERS" },
    [T_TRACK_WATCH] = { 220, "PLAYER %d IS CHOOSING" },         // what the other players see
    // In a session: the vehicle
    [T_CHOOSE_VEHICLE] = { MP_HEADING_Y, "CHOOSE A VEHICLE" },
    [T_VEHICLE_HELP] = { 220, "A: RACE   B: TRACKS" },
    [T_VEHICLE_WATCH] = { 220, "PLAYER %d IS CHOOSING" },
};

// ---- the tracks
//
// One line per track: the world it is listed under, the name shown, and the
// game's level. Tracks of the same world must be next to each other; each
// world is a page of the list. Which vehicles a track allows is the game's
// own knowledge and is not written here.
typedef struct MpTrack {
    char *world;
    char *name;
    s16 level;      // ASSET_LEVEL_* (include/asset_enums.h)
} MpTrack;

static MpTrack sMpTracks[] = {
    { "DINO DOMAIN", "ANCIENT LAKE", ASSET_LEVEL_ANCIENTLAKE },
    { "DINO DOMAIN", "FOSSIL CANYON", ASSET_LEVEL_FOSSILCANYON },
    { "DINO DOMAIN", "JUNGLE FALLS", ASSET_LEVEL_JUNGLEFALLS },
    { "DINO DOMAIN", "HOT TOP VOLCANO", ASSET_LEVEL_HOTTOPVOLCANO },
    { "SHERBET ISLAND", "WHALE BAY", ASSET_LEVEL_WHALEBAY },
    { "SHERBET ISLAND", "PIRATE LAGOON", ASSET_LEVEL_PIRATELAGOON },
    { "SHERBET ISLAND", "CRESCENT ISLAND", ASSET_LEVEL_CRESCENTISLAND },
    { "SHERBET ISLAND", "TREASURE CAVES", ASSET_LEVEL_TREASURECAVES },
    { "SNOWFLAKE MOUNTAIN", "EVERFROST PEAK", ASSET_LEVEL_EVERFROSTPEAK },
    { "SNOWFLAKE MOUNTAIN", "WALRUS COVE", ASSET_LEVEL_WALRUSCOVE },
    { "SNOWFLAKE MOUNTAIN", "SNOWBALL VALLEY", ASSET_LEVEL_SNOWBALLVALLEY },
    { "SNOWFLAKE MOUNTAIN", "FROSTY VILLAGE", ASSET_LEVEL_FROSTYVILLAGE },
    { "DRAGON FOREST", "BOULDER CANYON", ASSET_LEVEL_BOULDERCANYON },
    { "DRAGON FOREST", "GREENWOOD VILLAGE", ASSET_LEVEL_GREENWOODVILLAGE },
    { "DRAGON FOREST", "WINDMILL PLAINS", ASSET_LEVEL_WINDMILLPLAINS },
    { "DRAGON FOREST", "HAUNTED WOODS", ASSET_LEVEL_HAUNTEDWOODS },
    { "FUTURE FUN LAND", "SPACEDUST ALLEY", ASSET_LEVEL_SPACEDUSTALLEY },
    { "FUTURE FUN LAND", "DARKMOON CAVERNS", ASSET_LEVEL_DARKMOONCAVERNS },
    { "FUTURE FUN LAND", "STAR CITY", ASSET_LEVEL_STARCITY },
    { "FUTURE FUN LAND", "SPACEPORT ALPHA", ASSET_LEVEL_SPACEPORTALPHA },
};
#define MP_TRACK_COUNT ((s32) ARRAY_COUNT(sMpTracks))

// ---- the vehicles, in the game's order (car, hovercraft, plane)
static char *sMpVehicles[3] = { "CAR", "HOVERCRAFT", "PLANE" };
#define MP_VEHICLE_ROW_Y 96
#define MP_VEHICLE_ROW_STEP 28

// ===========================================================================
// From here on: how the screens behave.
// ===========================================================================

// ---- drawing

// Set by the test command HIDETEXT (3ds/autotest.c): the screens draw no
// words, for a picture of the sky alone.
int gMpHideText;

static void mp_heading(s32 id) {
    if (gMpHideText) {
        return;
    }
    set_text_font(ASSET_FONTS_BIGFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 128);
    draw_text(&sMenuCurrDisplayList, SCREEN_WIDTH_HALF + 1, sMpText[id].y + 3, sMpText[id].text, ALIGN_MIDDLE_CENTER);
    set_text_colour(255, 255, 255, 0, 255);
    draw_text(&sMenuCurrDisplayList, SCREEN_WIDTH_HALF, sMpText[id].y, sMpText[id].text, ALIGN_MIDDLE_CENTER);
    set_text_font(ASSET_FONTS_FUNFONT);
}

// A line of words at height y; the chosen one pulses like the game's menus.
static void mp_words(s32 y, char *text, s32 chosen, s32 opacity) {
    s32 highlight = 0;

    if (gMpHideText) {
        return;
    }
    if (chosen) {
        highlight = gOptionBlinkTimer * 8;
        if (gOptionBlinkTimer >= 32) {
            highlight = 511 - highlight;
        }
    }
    set_text_colour(255, 255, 255, highlight, opacity);
    draw_text(&sMenuCurrDisplayList, POS_CENTRED, y, text, ALIGN_MIDDLE_CENTER);
}

// A line of the table as it stands.
static void mp_line(s32 id, s32 chosen, s32 opacity) {
    mp_words(sMpText[id].y, sMpText[id].text, chosen, opacity);
}

// The dots that walk while something is awaited.
static char *mp_dots(void) {
    return &"..."[3 - ((gOptionBlinkTimer >> 4) & 3)];
}

static void mp_player_list(s32 y, s32 step) {
    char line[40];
    s32 count = netplay_lobby_player_count();
    s32 i;

    for (i = 0; i < NETPLAY_MAX_PLAYERS; i++) {
        if (i >= count) {
            sprintf(line, sMpText[T_EMPTY_ROW].text, i + 1);
            mp_words(y + i * step, line, FALSE, MP_EMPTY_SEAT);
        } else if (sLocalOnline) {
            sprintf(line, sMpText[T_PLAYER_ROW_RATED].text, i + 1, netplay_lobby_player_name(i),
                    netplay_lobby_player_rating(i));
            mp_words(y + i * step, line, FALSE, 255);
        } else {
            sprintf(line, sMpText[T_PLAYER_ROW].text, i + 1, netplay_lobby_player_name(i));
            mp_words(y + i * step, line, FALSE, 255);
        }
    }
}

// ===========================================================================
// Before a session: MULTIPLAYER, host or join, the lobby
// ===========================================================================

void menu_local_play_init(void) {
    gOptionBlinkTimer = 0;
    gMenuDelay = 0;
    transition_begin(&sMenuTransitionFadeOut);
    load_font(ASSET_FONTS_BIGFONT);
    music_voicelimit_set(24);
    music_play(SEQUENCE_MAIN_MENU);
    music_change_off();
    sLocalCursor = 0;
    sLocalStartFrames = 0;
    sLocalStage = LP_MODE;
}

static void mp_lobby_render(void) {
    char line[48];
    s32 count;
    s32 i;

    switch (sLocalStage) {
        case LP_MODE:
            mp_heading(T_MULTIPLAYER);
            mp_line(T_LOCAL, sLocalCursor == 0, 255);
            mp_line(T_ONLINE, sLocalCursor == 1, 255);
            mp_line(T_BACK, FALSE, MP_DIM);
            break;
        case LP_CHOICE:
            mp_heading(T_MULTIPLAYER);
            mp_line(T_HOST, sLocalCursor == 0, 255);
            mp_line(T_JOIN, sLocalCursor == 1, 255);
            mp_line(T_BACK, FALSE, MP_DIM);
            break;
        case LP_HOST:
            mp_heading(T_PLAYERS);
            if (sLocalOnline) {
                sprintf(line, sMpText[T_CODE].text, netplay_lobby_code());
                mp_words(sMpText[T_CODE].y, line, FALSE, 255);
                mp_player_list(84, 22);
                if (!netplay_lobby_code_is_public()) {
                    mp_line(T_PORT_SHUT, FALSE, 200);
                }
            } else {
                mp_player_list(MP_ROW_Y, MP_ROW_STEP);
            }
            if (netplay_lobby_player_count() >= 2 && netplay_lobby_state() == LOBBY_HOSTING) {
                mp_line(T_START, TRUE, 255);
            } else {
                mp_line(T_WAIT_PLAYERS, FALSE, 255);
            }
            mp_line(T_BACK_LOW, FALSE, MP_DIM);
            break;
        case LP_SEARCH:
            mp_heading(T_MULTIPLAYER);
            count = netplay_lobby_game_count();
            if (count == 0) {
                sprintf(line, sMpText[T_SEARCHING].text, mp_dots());
                mp_words(sMpText[T_SEARCHING].y, line, FALSE, 255);
            }
            for (i = 0; i < count && i < 4; i++) {
                sprintf(line, sMpText[T_GAME_ROW].text, netplay_lobby_game_name(i), netplay_lobby_game_players(i),
                        NETPLAY_MAX_PLAYERS);
                mp_words(MP_ROW_Y + i * MP_ROW_STEP, line, i == sLocalCursor, 255);
            }
            mp_line(count != 0 ? T_JOIN_OR_BACK : T_BACK_LOW, FALSE, MP_DIM);
            break;
        case LP_JOINED:
            mp_heading(T_PLAYERS);
            if (netplay_lobby_player_count() == 0) {
                // Online: nothing has come back from the host yet.
                sprintf(line, sMpText[T_CONNECTING].text, mp_dots());
                mp_words(sMpText[T_CONNECTING].y, line, FALSE, 255);
            } else {
                mp_player_list(MP_ROW_Y, MP_ROW_STEP);
                mp_line(T_WAIT_HOST, FALSE, 255);
            }
            mp_line(T_BACK_LOW, FALSE, MP_DIM);
            break;
        case LP_NO_SERVICE:
            mp_heading(T_MULTIPLAYER);
            mp_line(sLocalOnline ? T_NO_INTERNET_1 : T_NO_WIRELESS_1, FALSE, 255);
            mp_line(sLocalOnline ? T_NO_INTERNET_2 : T_NO_WIRELESS_2, FALSE, 255);
            mp_line(T_BACK_LOW, FALSE, MP_DIM);
            break;
        case LP_NO_GAME:
            mp_heading(T_MULTIPLAYER);
            mp_line(T_NO_GAME_1, FALSE, 255);
            mp_line(T_NO_GAME_2, FALSE, 255);
            mp_line(T_BACK_LOW, FALSE, MP_DIM);
            break;
        case LP_STARTING:
            mp_heading(T_MULTIPLAYER);
            mp_line(T_GET_READY, TRUE, 255);
            break;
    }
}

/**
 * Every console runs this at the same poll of the session: straight to the
 * character select with every player in, the way the title screen goes
 * there with one.
 */
static void localplay_go_to_character_select(void) {
    s32 players = netplay_players();
    s32 cutscene = 0;
    s32 i;

    unload_font(ASSET_FONTS_BIGFONT);
    set_rng_seed((s32) netplay_seed());
    reset_character_id_slots();
    gSaveFileIndex = 0;
    gNumberOfActivePlayers = players;
    for (i = 0; i < ARRAY_COUNT(gPlayersCharacterArray); i++) {
        gPlayersCharacterArray[i] = (i < players) ? i : -1;
    }
    for (i = 0; i < ARRAY_COUNT(gActivePlayersArray); i++) {
        gActivePlayersArray[i] = (i < players) ? TRUE : FALSE;
    }
    set_time_trial_enabled(FALSE);
    gIsInTracksMode = TRUE;
    // Computer racers fill the grid unless the players choose otherwise
    // (0, 1, 2 = two, four, six racers).
    gMultiplayerSelectedNumberOfRacers = 2;
    if (is_drumstick_unlocked()) {
        cutscene = 1;
    }
    if (is_tt_unlocked()) {
        cutscene ^= 3;
    }
    music_change_on();
    load_level_for_menu(ASSET_LEVEL_CHARACTERSELECT, -1, cutscene);
    charselect_prev(0, NULL);
    menu_init(MENU_CHARACTER_SELECT);
}

s32 menu_local_play_loop(s32 updateRate) {
    char code[24];
    s32 buttonsPressed = 0;
    s32 analogueY = 0;
    s32 count;
    s32 i;

    gOptionBlinkTimer = (gOptionBlinkTimer + updateRate) & 0x3F;
    if (gMenuDelay < 0) {
        gMenuDelay -= updateRate;
    }
    if (gMenuDelay > -20) {
        mp_lobby_render();
    }

    // The session has begun: nothing here depends on this console alone
    // from now on, and the frames are counted, not timed.
    if (sLocalStage == LP_STARTING) {
        sLocalStartFrames++;
        if (sLocalStartFrames >= LP_START_FRAMES) {
            localplay_go_to_character_select();
        }
        return MENU_RESULT_CONTINUE;
    }

    if (gMenuDelay < -30) {
        // Back to the title screen.
        netplay_lobby_leave();
        music_change_on();
        unload_font(ASSET_FONTS_BIGFONT);
        menu_init(MENU_TITLE);
        return MENU_RESULT_CONTINUE;
    }

    netplay_lobby_tick();
    if (netplay_lobby_state() == LOBBY_SESSION) {
        localplay_session_open();
        sLocalStage = LP_STARTING;
        sLocalStartFrames = 0;
        transition_begin(&sMenuTransitionFadeIn);
        sound_play(SOUND_SELECT2, NULL);
        return MENU_RESULT_CONTINUE;
    }
    // The other side can change where this console is: a host that closes
    // its game (or never answered) sends a joiner back.
    if (sLocalStage == LP_JOINED && netplay_lobby_state() == LOBBY_SEARCHING) {
        sLocalStage = sLocalOnline ? LP_NO_GAME : LP_SEARCH;
        sLocalCursor = 0;
        sound_play(SOUND_MENU_BACK3, NULL);
    }

    if (gIgnorePlayerInputTime == 0 && gMenuDelay == 0) {
        for (i = 0; i < MAXCONTROLLERS; i++) {
            buttonsPressed |= input_pressed(i);
            analogueY += gControllersYAxisDirection[i];
        }
    }
    gIgnorePlayerInputTime = 0;

    switch (sLocalStage) {
        case LP_MODE:
            if (analogueY != 0) {
                sLocalCursor ^= 1;
                sound_play(SOUND_MENU_PICK2, NULL);
            }
            if (buttonsPressed & (A_BUTTON | START_BUTTON)) {
                sLocalOnline = sLocalCursor;
                sLocalStage = netplay_lobby_enter(sLocalOnline) ? LP_CHOICE : LP_NO_SERVICE;
                sLocalCursor = 0;
                sound_play(SOUND_SELECT2, NULL);
            } else if (buttonsPressed & B_BUTTON) {
                music_fade(-128);
                gMenuDelay = -1;
                transition_begin(&sMenuTransitionFadeIn);
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_CHOICE:
            if (analogueY != 0) {
                sLocalCursor ^= 1;
                sound_play(SOUND_MENU_PICK2, NULL);
            }
            if (buttonsPressed & (A_BUTTON | START_BUTTON)) {
                if (sLocalCursor == 0) {
                    if (netplay_lobby_host()) {
                        sLocalStage = LP_HOST;
                        sound_play(SOUND_SELECT2, NULL);
                    } else {
                        sLocalStage = LP_NO_SERVICE;
                    }
                } else if (sLocalOnline) {
                    // The console's keyboard takes the code.
                    if (netplay_ask_code(code, sizeof(code))) {
                        if (netplay_lobby_join_code(code)) {
                            sLocalStage = LP_JOINED;
                            sound_play(SOUND_SELECT2, NULL);
                        } else {
                            sLocalStage = LP_NO_GAME;
                            sound_play(SOUND_MENU_BACK3, NULL);
                        }
                    }
                } else {
                    netplay_lobby_search();
                    sLocalStage = LP_SEARCH;
                    sLocalCursor = 0;
                    sound_play(SOUND_SELECT2, NULL);
                }
            } else if (buttonsPressed & B_BUTTON) {
                netplay_lobby_leave();
                sLocalStage = LP_MODE;
                sLocalCursor = sLocalOnline;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_HOST:
            if ((buttonsPressed & (A_BUTTON | START_BUTTON)) && netplay_lobby_state() == LOBBY_HOSTING) {
                if (netplay_lobby_start()) {
                    sound_play(SOUND_SELECT2, NULL);
                }
            } else if (buttonsPressed & B_BUTTON) {
                netplay_lobby_search();     // closes the game
                sLocalStage = LP_CHOICE;
                sLocalCursor = 0;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_SEARCH:
            count = netplay_lobby_game_count();
            if (count > 4) {
                count = 4;
            }
            if (sLocalCursor >= count) {
                sLocalCursor = count > 0 ? count - 1 : 0;
            }
            if (analogueY < 0 && sLocalCursor < count - 1) {
                sLocalCursor++;
                sound_play(SOUND_MENU_PICK2, NULL);
            } else if (analogueY > 0 && sLocalCursor > 0) {
                sLocalCursor--;
                sound_play(SOUND_MENU_PICK2, NULL);
            }
            if ((buttonsPressed & (A_BUTTON | START_BUTTON)) && count > 0) {
                if (netplay_lobby_join(sLocalCursor)) {
                    sLocalStage = LP_JOINED;
                    sound_play(SOUND_SELECT2, NULL);
                } else {
                    sound_play(SOUND_MENU_BACK3, NULL);
                }
            } else if (buttonsPressed & B_BUTTON) {
                sLocalStage = LP_CHOICE;
                sLocalCursor = 1;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_JOINED:
            if (buttonsPressed & B_BUTTON) {
                netplay_lobby_search();     // leaves the game
                sLocalStage = sLocalOnline ? LP_CHOICE : LP_SEARCH;
                sLocalCursor = sLocalOnline ? 1 : 0;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_NO_GAME:
            if (buttonsPressed & (A_BUTTON | B_BUTTON | START_BUTTON)) {
                sLocalStage = LP_CHOICE;
                sLocalCursor = 1;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
        case LP_NO_SERVICE:
            if (buttonsPressed & (A_BUTTON | B_BUTTON | START_BUTTON)) {
                netplay_lobby_leave();
                sLocalStage = LP_MODE;
                sLocalCursor = sLocalOnline;
                sound_play(SOUND_MENU_BACK3, NULL);
            }
            break;
    }
    return MENU_RESULT_CONTINUE;
}

// ===========================================================================
// In a session: the track and the vehicle (MENU_SESSION_TRACKS)
//
// Every console runs this on the same controller samples, so every console
// is on the same line of the same page. The player who chooses (the host,
// then each race's winner: localplay_chooser) is the only one whose pad is
// read; menu_input hands it over as controller 0.
// ===========================================================================

enum { MPS_TRACK, MPS_VEHICLE };
static s32 sMpStage;
static s32 sMpTrack;        // the line of sMpTracks the cursor is on, or one of the two below
#define MP_ON_RANDOM MP_TRACK_COUNT
#define MP_ON_MORE_PLAYERS (MP_TRACK_COUNT + 1)
static s32 sMpPageTrack;    // a track of the page shown (RANDOM belongs to every page)
static s32 sMpVehicle;      // 0 car, 1 hovercraft, 2 plane
static s32 sMpChosenTrack;  // what the race will be on

// Whether two tracks are listed under the same world (the same words).
static s32 mp_same_world(s32 a, s32 b) {
    char *x = sMpTracks[a].world;
    char *y = sMpTracks[b].world;

    while (*x != 0 && *x == *y) {
        x++;
        y++;
    }
    return *x == *y;
}

// The first and one past the last track of the page that track `track` is on.
static void mp_page_of(s32 track, s32 *first, s32 *end) {
    *first = track;
    while (*first > 0 && mp_same_world(*first - 1, track)) {
        (*first)--;
    }
    *end = track + 1;
    while (*end < MP_TRACK_COUNT && mp_same_world(*end, track)) {
        (*end)++;
    }
}

static s32 mp_vehicle_allowed(s32 vehicle) {
    return (leveltable_vehicle_usable(sMpTracks[sMpChosenTrack].level) >> vehicle) & 1;
}

void menu_session_tracks_init(void) {
    gOptionBlinkTimer = 0;
    gMenuDelay = 0;
    load_level_for_menu(MP_BACKGROUND, -1, 0);
    transition_begin(&sMenuTransitionFadeOut);
    load_font(ASSET_FONTS_BIGFONT);
    music_voicelimit_set(24);
    music_play(SEQUENCE_MAIN_MENU);
    music_change_off();
    sMpStage = MPS_TRACK;
    if (sMpPageTrack < 0 || sMpPageTrack >= MP_TRACK_COUNT) {
        sMpPageTrack = 0;
    }
    if (sMpTrack < 0 || sMpTrack >= MP_TRACK_COUNT) {
        sMpTrack = sMpPageTrack;    // not left on RANDOM or on the way to the lobby
    }
}

static void mp_session_render(void) {
    char line[48];
    s32 first, end;
    s32 i, y;

    if (sMpStage == MPS_TRACK) {
        mp_page_of(sMpPageTrack, &first, &end);
        mp_heading(T_CHOOSE_TRACK);
        sprintf(line, sMpText[T_WORLD].text, sMpTracks[first].world);
        mp_words(sMpText[T_WORLD].y, line, FALSE, 255);
        for (i = first, y = MP_TRACK_ROW_Y; i < end; i++, y += MP_TRACK_ROW_STEP) {
            mp_words(y, sMpTracks[i].name, i == sMpTrack, i == sMpTrack ? 255 : MP_UNCHOSEN);
        }
        mp_line(T_RANDOM, sMpTrack == MP_ON_RANDOM, sMpTrack == MP_ON_RANDOM ? 255 : MP_UNCHOSEN);
        mp_line(T_MORE_PLAYERS, sMpTrack == MP_ON_MORE_PLAYERS, sMpTrack == MP_ON_MORE_PLAYERS ? 255 : MP_UNCHOSEN);
        if (localplay_shows_player(localplay_chooser())) {
            mp_line(T_TRACK_HELP, FALSE, MP_DIM);
        } else {
            sprintf(line, sMpText[T_TRACK_WATCH].text, localplay_chooser() + 1);
            mp_words(sMpText[T_TRACK_WATCH].y, line, FALSE, MP_DIM);
        }
    } else {
        mp_heading(T_CHOOSE_VEHICLE);
        mp_words(sMpText[T_WORLD].y, sMpTracks[sMpChosenTrack].name, FALSE, 255);
        for (i = 0, y = MP_VEHICLE_ROW_Y; i < 3; i++) {
            if (mp_vehicle_allowed(i)) {
                mp_words(y, sMpVehicles[i], i == sMpVehicle, i == sMpVehicle ? 255 : MP_UNCHOSEN);
                y += MP_VEHICLE_ROW_STEP;
            }
        }
        if (localplay_shows_player(localplay_chooser())) {
            mp_line(T_VEHICLE_HELP, FALSE, MP_DIM);
        } else {
            sprintf(line, sMpText[T_VEHICLE_WATCH].text, localplay_chooser() + 1);
            mp_words(sMpText[T_VEHICLE_WATCH].y, line, FALSE, MP_DIM);
        }
    }
}

// The track is settled: on to its vehicles, starting on the track's usual one.
static void mp_track_chosen(s32 track) {
    sMpChosenTrack = track;
    sMpPageTrack = track;
    sMpVehicle = leveltable_vehicle_default(sMpTracks[track].level);
    if (sMpVehicle < 0 || sMpVehicle > 2 || !mp_vehicle_allowed(sMpVehicle)) {
        for (sMpVehicle = 0; sMpVehicle < 2 && !mp_vehicle_allowed(sMpVehicle); sMpVehicle++) {}
    }
    sMpStage = MPS_VEHICLE;
    sound_play(SOUND_SELECT2, NULL);
}

// Hands the choice to the game the way its own track select does.
static s32 mp_start_race(void) {
    s32 i;

    unload_font(ASSET_FONTS_BIGFONT);
    // The menu's music was held on (music_change_off in the init); let go of
    // it, or it plays on through the race instead of the track's own.
    music_change_on();
    music_stop();
    gTrackIdToLoad = sMpTracks[sMpChosenTrack].level;
    gTrackIdForPreview = gTrackIdToLoad;
    gTrackSpecifiedWithTrackIdToLoad = 1;
    for (i = 0; i < ARRAY_COUNT(gPlayerSelectVehicle); i++) {
        gPlayerSelectVehicle[i] = sMpVehicle;
    }
    set_level_default_vehicle((enum Vehicle) sMpVehicle);
    set_time_trial_enabled(FALSE);
    gIsInAdventureTwo = FALSE;
    return gNumberOfActivePlayers;
}

// Back to the character select, as the game's track select goes with several players.
static void mp_back_to_characters(void) {
    s32 cutscene = 0;

    unload_font(ASSET_FONTS_BIGFONT);
    if (is_drumstick_unlocked()) {
        cutscene = 1;
    }
    if (is_tt_unlocked()) {
        cutscene ^= 3;
    }
    music_change_on();
    load_level_for_menu(ASSET_LEVEL_CHARACTERSELECT, -1, cutscene);
    charselect_prev(0, NULL);
    menu_init(MENU_CHARACTER_SELECT);
}

// Everybody back to the lobby, where more players can join: the session
// stops here on every console at once, and the connection stays.
static void mp_back_to_lobby(void) {
    unload_font(ASSET_FONTS_BIGFONT);
    localplay_session_to_lobby();
    sound_play(SOUND_SELECT2, NULL);
    menu_init(MENU_LOCAL_PLAY);
    sLocalStage = netplay_lobby_state() == LOBBY_HOSTING ? LP_HOST : LP_JOINED;
}

s32 menu_session_tracks_loop(s32 updateRate) {
    s32 first, end;
    s32 buttons, stickX, stickY;
    s32 step;

    gOptionBlinkTimer = (gOptionBlinkTimer + updateRate) & 0x3F;
    menu_input();
    if (gMenuDelay > -20 && gMenuDelay < 20) {
        mp_session_render();
    }
    // Leaving: the screen fades for half a second first.
    if (gMenuDelay > 0) {
        gMenuDelay += updateRate;
        return gMenuDelay > 30 ? mp_start_race() : MENU_RESULT_CONTINUE;
    }
    if (gMenuDelay < 0) {
        gMenuDelay -= updateRate;
        if (gMenuDelay < -30) {
            mp_back_to_characters();
        }
        return MENU_RESULT_CONTINUE;
    }
    buttons = gIgnorePlayerInputTime == 0 ? gMenuButtons[PLAYER_ONE] : 0;
    stickX = gIgnorePlayerInputTime == 0 ? gMenuStickX[PLAYER_ONE] : 0;
    stickY = gIgnorePlayerInputTime == 0 ? gMenuStickY[PLAYER_ONE] : 0;
    gIgnorePlayerInputTime = 0;

    if (sMpStage == MPS_TRACK) {
        mp_page_of(sMpPageTrack, &first, &end);
        if (stickY != 0) {
            // Up and down the page's tracks and the two lines under them, round and round.
            s32 tracks = end - first;
            s32 rows = tracks + 2;
            s32 row = sMpTrack >= MP_TRACK_COUNT ? tracks + (sMpTrack - MP_TRACK_COUNT) : sMpTrack - first;

            row = (row + (stickY < 0 ? 1 : rows - 1)) % rows;
            sMpTrack = row >= tracks ? MP_TRACK_COUNT + (row - tracks) : first + row;
            sound_play(SOUND_MENU_PICK2, NULL);
        } else if (stickX != 0) {
            // Left and right through the worlds, keeping the cursor's row.
            s32 row = sMpTrack >= MP_TRACK_COUNT ? -1 : sMpTrack - first;

            sMpPageTrack = stickX > 0 ? (end < MP_TRACK_COUNT ? end : 0) : (first > 0 ? first - 1 : MP_TRACK_COUNT - 1);
            mp_page_of(sMpPageTrack, &first, &end);
            if (row >= 0) {
                sMpTrack = first + row < end ? first + row : end - 1;
            }
            sound_play(SOUND_MENU_PICK2, NULL);
        }
        if ((buttons & (A_BUTTON | START_BUTTON)) && sMpTrack == MP_ON_MORE_PLAYERS) {
            mp_back_to_lobby();
            return MENU_RESULT_CONTINUE;
        }
        if ((buttons & L_TRIG) || ((buttons & (A_BUTTON | START_BUTTON)) && sMpTrack == MP_ON_RANDOM)) {
            // The game's own random numbers: the same on every console.
            mp_track_chosen(rand_range(0, MP_TRACK_COUNT - 1));
        } else if (buttons & (A_BUTTON | START_BUTTON)) {
            mp_track_chosen(sMpTrack);
        } else if (buttons & B_BUTTON) {
            gMenuDelay = -1;
            transition_begin(&sMenuTransitionFadeIn);
            sound_play(SOUND_MENU_BACK3, NULL);
        }
    } else {
        if (stickY != 0) {
            step = stickY < 0 ? 1 : 2;
            do {
                sMpVehicle = (sMpVehicle + step) % 3;
            } while (!mp_vehicle_allowed(sMpVehicle));
            sound_play(SOUND_MENU_PICK2, NULL);
        }
        if (buttons & (A_BUTTON | START_BUTTON)) {
            gMenuDelay = 1;
            disable_new_screen_transitions();
            transition_begin(&sMenuTransitionFadeIn);
            sound_play(SOUND_SELECT2, NULL);
        } else if (buttons & B_BUTTON) {
            sMpStage = MPS_TRACK;
            sound_play(SOUND_MENU_BACK3, NULL);
        }
    }
    return MENU_RESULT_CONTINUE;
}
