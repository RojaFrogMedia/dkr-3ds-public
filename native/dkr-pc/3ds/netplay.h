// Local Play between consoles (netplay.c). Plain C types only, so that the
// game's code (src/menu.c through 3ds/localplay_menu.inc, 3ds/reimpl.c) can
// include it next to its own type names.
#ifndef DKR_3DS_NETPLAY_H
#define DKR_3DS_NETPLAY_H

#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_NAME_CHARS 10

// ---- the lobby: before a session

enum {
    LOBBY_OFF,          // not in Local Play
    LOBBY_HOSTING,      // this console's game is open for others to join
    LOBBY_SEARCHING,    // looking for games
    LOBBY_JOINED,       // in somebody's game, waiting for the host to start
    LOBBY_STARTING,     // the host has started: everybody is being told
    LOBBY_SESSION,      // the session runs; the lobby is over
};

// Starts the service: local wireless, or the internet when `online` is set.
// 0 when the console has none to use.
int netplay_lobby_enter(int online);
// Leaves Local Play altogether: any game hosted or joined, and the service.
void netplay_lobby_leave(void);
// Once a frame while the lobby screen is up: sends and receives.
void netplay_lobby_tick(void);
int netplay_lobby_state(void);

int netplay_lobby_host(void);               // open a game; 0 on failure
int netplay_lobby_search(void);             // start looking (also: leave a joined game)
int netplay_lobby_game_count(void);         // games found by the search
const char *netplay_lobby_game_name(int game);
int netplay_lobby_game_players(int game);
int netplay_lobby_join(int game);           // 0 on failure

// Online: a hosted game's code ("" when not hosting online), whether it is
// good from the internet or only on the host's own network, asking the
// player for a code, and joining by one (0: not a code).
const char *netplay_lobby_code(void);
int netplay_lobby_code_is_public(void);
int netplay_ask_code(char *out, int size);
int netplay_lobby_join_code(const char *text);

int netplay_lobby_player_count(void);       // in the game hosted or joined
const char *netplay_lobby_player_name(int slot);
int netplay_lobby_start(void);              // host only, with two players or more

// ---- the session

int netplay_players(void);          // 0: no session
int netplay_slot(void);             // this console's player, 0-based
const char *netplay_player_name(int player);    // a session's player, "" outside one
unsigned netplay_session_roster(void);  // the added characters all its consoles have (3ds/characters.c), or 0
int netplay_fixed_rate(void);       // the session's logic rate (1 = 60 Hz, 2 = 30 Hz), 0 without one
unsigned netplay_seed(void);        // the random seed every console starts from
unsigned netplay_poll_count(void);  // controller polls since the session began

// One controller poll: this console's pad in, every player's out. Blocks
// until all of them are known for this poll.
void netplay_poll(unsigned short buttons, signed char x, signed char y, unsigned short *outButtons,
                  signed char *outX, signed char *outY);

// Points ("rating" in the code; see the points section of netplay.c): from
// 0, and they go below it. Online they are the profile's; a local wireless
// session keeps its own. The game reports each race once, at the same poll
// on every console, with each player's finishing position (1 = first, 0 =
// not racing).
int netplay_session_online(void);
int netplay_rating(int player);             // a session's player, or this console's own outside one
int netplay_rating_change(int player);      // what the last race did to it
int netplay_my_rating(void);
int netplay_lobby_player_rating(int slot);
void netplay_report_race(const int places[NETPLAY_MAX_PLAYERS]);

// Idle players (the game decides, on every console at the same poll): the
// player the touch screens should warn about (1-based, 0 for nobody), and
// taking a player out of the session for good.
void netplay_set_idle_warning(int player);
int netplay_idle_warning(void);
void netplay_drop_player(int player);
int netplay_player_dropped(int player);

// Ends the session on this console and tells the others.
void netplay_end(void);
// The session stops but the game stays open: every console calls this at the
// same poll, and all are back in the lobby they started from, where more
// players can join before the host starts again.
void netplay_back_to_lobby(void);
// 1 once after a session ended because the others went away.
int netplay_take_lost(void);

void netplay_init(const char *dir);

#endif
