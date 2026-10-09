# The Nintendo 3DS target

The game is the decompilation in `src/` and `libultra/`, compiled natively.
This folder is everything that is specific to the 3DS. Game code changes for
the 3DS are inside `#ifdef TARGET_3DS` (or `TARGET_PC`, shared with the
Linux and Dreamcast targets); a search for `TARGET_3DS` lists all of them.

## How a frame happens

```
game thread (core 0)                     render thread          audio mixer
--------------------                     -------------          -----------
thread3_main -> main_game_loop
  game logic, builds a display list
  gfxtask_run_xbus -> pc_gfx_task_submit
      pc_render_post  ------------------> pc_gfx_task_run
      pc_audio_frame                        run_dl (interpret, record)
        alAudioFrame (command list)         gfx_frame_end (hand to the GPU)
        pc_audio_hle_run  ------------------------------------> mix, queue on NDSP
  fb_update -> pc_retrace_wait
      (waits for the screen, returns the timestep)
```

The game alternates between two sets of display-list buffers, as it did for
the N64's graphics chip, so the render thread may still be drawing frame N
while the game builds frame N+1. A new list waits for the one in flight.

Three things work at once, each one frame behind the one before it: the
game builds frame N+2, the render thread interprets frame N+1, the GPU draws
frame N. For the last two to overlap, the interpreter sends nothing to the
GPU while it runs: vertices go into one of two vertex buffers and each run
of triangles with one state becomes a record (`DrawRecord` in `gfx.c`).
`gfx_frame_end` then waits for the GPU to finish frame N, turns the records
into commands and starts it on frame N+1. Before this, commands were sent as
the list was interpreted, which meant waiting for the GPU first: a frame cost
the interpreter's time plus the GPU's, and on a New 3DS the heaviest races
ran at 40-47 fps with 13 ms of measured work.

Pacing is by the screen's own vertical blank (`gfx_vblank_wait`, counted by a
callback in `gfx.c`): the game thread starts a frame on a blank, and the
timestep is the number of blanks the last frame was on screen for. Nothing is
paced by a timer.

## Files

| File | What it is | Change it when |
|---|---|---|
| `platform.c` | `main`, frame pacing (`pc_retrace_wait`), the render thread | the 60/30 decision, start-up order, thread placement |
| `main.c` | The F3DDKR display-list interpreter: matrices, vertices, triangles, textures, combiner, rectangles, widescreen, per-console view | something draws wrong, or drawing is slow |
| `gfx.c`, `gfx.h` | The GPU layer (citro3d): textures, texture-environment stages, depth, scissor, the frame's vertex buffer | a GPU state is missing or wrong |
| `dkr.v.pica` | The one vertex shader | vertex format changes |
| `audio.c` | NDSP output, the mixer thread, `osAi*` | audio stutters or lags |
| `audio_hle.c` | The audio command-list mixer (shared with the Linux target) | a sound is wrong |
| `input.c`, `cpp.c` | Pad to N64 controller; New 3DS second pad; Circle Pad Pro | controls |
| `touchmenu.c`, `touchmenu.h` | The settings menu on the touch screen (SELECT), `settings.ini` | a new setting: a variable, a line in `sRows`, a getter |
| `bottomscreen.c`, `bottom.h`, `bottom_game.inc` | The touch screen while that menu is closed: the race display, the Adventure's collectables, the logo and banana bank. `bottom_game.inc` is the game's side, compiled into `src/game_ui.c` | what the touch screen shows, and where on it |
| `hardmode.c` | Hard mode: the computer racers' schedule from T.T.'s and record times | how hard, which tracks |
| `characters.c`, `characters.h` | Added characters (from character patches) and the banana bank that unlocks them | a rule about unlocking, voices, who is available |
| `reimpl.c` | The N64 OS calls the game makes: asset reads, controllers, EEPROM save, timers | saves, asset loading |
| `netplay.c`, `netplay.h` | Local Play between consoles: the lobby (host, search, join, start) and the session, controller exchange in lockstep | multiplayer rules, the protocol |
| `link.c`, `link.h` | The packets under it: local wireless (UDS), UDP across the internet to a host's address, or UDP on an access point for the emulator tests | a connection problem |
| `upnp.c`, `upnp.h` | Asks the home router to let an online game in (port forwarding, public address) and takes it back afterwards | hosting online fails behind some router |
| `localplay_menu.inc` | The Multiplayer screen and the session's hooks into the game, compiled as part of `src/menu.c` | what the lobby looks like, what a session sets up |
| `diag.c`, `diag.h` | `CRASH` and `STALL` reports in the log | you need to know where it died |
| `autotest.c` | `log.txt`, scripted input, screenshots, the once-a-second `stats`/`prof` lines | test runs |

`main.c`, `reimpl.c` and `audio_hle.c` started as copies of the files in
`linux/` and are built with the game's headers, not libctru's (the two
define `u32` differently). Anything that needs libctru goes in one of the
other files and is reached through a plain C function.

## Reading the log

`sdmc:/3ds/DKR/log.txt`, once a second:

```
stats: 60 fps, frame work avg 8.7 ms max 11.1 ms
prof: per frame: display list 8.4 ms, present 0.3 ms, audio 0.1 ms, 2049 vertices
```

`frame work` is the larger of the game thread's time and the render thread's
time; above 16.7 ms the game commits to 30 fps until it has had a second and
a half of frames under 14 ms. `display list` is the interpreter, `present` the
hand-over to the GPU, `audio` the synthesizer on the game thread (the mixer
is not in it). Then:

```
gpu: per frame: 6.1 ms (longest 8.0), waited for 0.2 ms; 1 frames late by 1 refreshes
late: 2 refreshes for one frame: game 3.1 ms, waited for the render thread 14.2 ms, its frame 17.5 ms (of that waiting for the GPU 6.0 ms)
```

`gpu` is the GPU's own time for a frame. It runs beside the frame work, not
inside it, so it only matters when it nears 16 ms, or when `waited for` (how
long the hand-over stood waiting for the GPU) is more than a fraction of a
millisecond. `late` counts frames that stayed on screen longer than the
committed rate: every one is a visible hitch, and a second without any is
exactly 60 (or 30) frames. The first three late frames of each second get a
`late:` line saying whose time it was: the game's own (a level load, a save),
the render thread's (many new textures at once), or the GPU's.

```
clip: in 60 frames 158 flat draws (text, pictures) and 336 of the world had an empty clip rectangle
```

Only written when there were any: draws that had nowhere to show and were
left out. A few while the track select scrolls are its neighbours off the
screen. Many on a screen whose words are missing is that screen's bug: text,
pictures and boxes are rectangles, which only the game's scissor may cut
(`CLIP_RECT`, `CLIP_FULL` in `main.c`), never the viewport of the view drawn
before them. When they were cut by both, the pause menu in a race, the
multiplayer screens and the character select's heading were not drawn at all
(`native/tests/racepause.txt`, `localplay-ui.txt`, `menus.txt` show them).

The log is written to the card by a thread of its own (`log_thread` in
`autotest.c`). When the game thread wrote it, each once-a-second report cost
a console a frame or two, which is the 58 fps every earlier console log shows
whatever the frame cost. `pc_log_sync` puts what is buffered on the card at
once; crash, stall and trail reports call it.

`make -f Makefile.3ds BUILD=$HOME/dkr-prof-build PROFILE=1` adds a `dlprof:`
line every 60 frames: the interpreter's time split into vertex loads,
polygons, combiner models worked out, texture look-ups, matrices and
rectangles.

## When it freezes or crashes

`diag.c` writes one of two reports to the log:

```
STALL: no frame for 3 seconds
  game thread: game logic (frame 28783)
  render thread: idle (frame 28753)
  audio mixer: idle
  game thread call stack, outermost first (addr2line -f -e dkracing.elf <address>):
    00196410 00196014 00188748 0010d8dc 0010d7f4 001b5830 001b2f40 001b1098
```

The call stack is only there in a tracer build, which is the same source
built into another directory with `TRACE=1` (every function of the game and
libultra then records itself on entry; about a tenth slower):

```
make -f Makefile.3ds BUILD=$HOME/dkr-trace-build TRACE=1 -j14
DKR_BUILD=dkr-trace-build tools/run-native.sh native/tests/long.txt 760
arm-none-eabi-addr2line -f -e $HOME/dkr-trace-build/dkracing.elf 00196410 ...
```

`CRASH:` lines carry the faulting `pc` and `lr` for the same `addr2line`.
The emulator does not deliver exceptions to the program, so those appear on
a console only.

The freeze this was built for: libultra's sequence players keep themselves
alive with events in their own queue, the queue drops events when full, and
an empty queue made `__CSPVoiceHandler` loop forever. See
`__CSPRestoreOwnEvents` in `libultra/src/audio/mips1/csplayer.c`. A line
`AUDIO: n sequencer events dropped` in the log means the queues
(`src/audio.c`, `audio_init`) are too short again.

## Sound that goes missing

libultra's audio has three fixed pools that the game fills between two audio
frames: each sequence player's event queue, the sound effect player's event
queue, and the synthesizer's voice updates (every start, stop, volume, pitch
and pan is one). When one is empty the request is dropped without a word:
music stops, an engine loop never starts, a voice is never given back. The
3DS runs more game frames per audio frame than the N64 did, so all three are
larger here (`PC_EVTQ_SCALE` in `src/audio.c`). The log says so if it ever
happens again:

```
AUDIO: 12 sequencer events dropped (queue full)
AUDIO: 37 voice updates dropped (pool empty)
```

and the once-a-second `sound:` line shows how many sounds play against the
limit and how the synthesizer's 40 voices are used.

## HOME, sleep and leaving

While the program is suspended the GPU is the system's. `apt_hook` in
`platform.c` finishes the frame in flight before the system takes over, and
`platform_shutdown` (run by `exit()`) closes audio and graphics in order.
Without them, closing the game from the HOME menu crashed the console.

The HOME button pauses first (`home_pause` in `platform.c`): the HOME menu
is never allowed (`aptSetHomeAllowed(false)`), so a press arrives as
"refused" (`aptCheckHomePressRejected`). The game then stands still with the
render and audio threads idle and the touch screen says PAUSED. A second
press of HOME closes the game itself, the ordinary way out
(`platform_shutdown`), back to the Homebrew Launcher or the HOME menu; any
other button goes back to the game. The game used to hand over to the HOME
menu on the second press, and every run of a console that froze ended with
the trail at "HOME menu: open": handed over, and never run again. The emulator has no HOME menu; `native/tests/homepause.txt`
(test command `HOME`) only shows the pause and the return from it.

Nothing in these paths may wait without end: the waits for the render
thread and the sound mixer give up after a second (`render_wait_idle_bounded`,
`pc_audio_wait_idle_bounded`), and the save and the banana bank are written
when the pause begins, so closing from the HOME menu has nothing left to do.

**The trail.** The watchdog is silent while the program is paused or
suspended, so a freeze there used to leave nothing in the log. Now every
step of these paths writes one line to `state.txt` (`diag_trail` in `diag.c`,
written and closed at once) and to the log as `TRAIL:`. A run that ends
properly leaves `ended`. If the next start finds anything else it keeps the
old log as `log-prev.txt` and its own log begins with
`LAST RUN: did not end; it stopped at or after "..."`. After a freeze, read
`state.txt` first: `running` means the freeze was in the game itself (look
for `STALL` or `CRASH` in `log-prev.txt`), anything else names the step.

## The touch screen

While the settings menu is closed the touch screen shows one of three pages
(`bottomscreen.c`; the numbers under "layout" at its top place everything):

| When | What |
|---|---|
| A race, a boss, a challenge | The standings: place, portrait and name of every racer, this console's own row in gold; a row slides up or down when its racer gains or loses a place. The track's map with every racer on it. Speed (the number the game's own dial points at, 0 to 150), lap, bananas with the banana picture, silver coins in a silver coin race. In the Adventure a line with balloons, amulet pieces and trophies |
| Driving around the Adventure's island or a world's lobby | What the save has collected: golden balloons of 47, Wizpig amulet pieces, T.T. amulet pieces, keys, trophies, boss races won, each with a bar; and the banana bank |
| Anything else (menus, cutscenes, the demos) | The logo and the banana bank with the banana picture |

A session between consoles keeps its own notices on the menus (which player
the console is, the game's code, the idle warning); in its races the
standings carry each player's name and points beside the portrait, and
computer racers say CPU.

What the touch screen shows in a race is taken off the top screen: the
banana count (`hud_bananas`) and the map (`hud_render_general`), both
through `bottom_has_race` in `bottom_game.inc`. The hub's map stays on the
top screen, the touch screen having none there. If "Touch screen: Off in
play" is chosen in the settings, the two are on neither screen.

How it gets its facts: once a frame, where the game draws its own HUD
(`hud_render_general` in `src/game_ui.c`), `bottom_update` fills in a
snapshot (`BottomSnapshot` in `bottom.h`): the racers with place, character,
portrait and place on the map, this console's speed, bananas and lap, the
map as a picture, the save's collectables. A snapshot that stops changing
means no level is being played, and the page goes back to the logo. The map
is the level's own map sprite copied out (`bottom_build_map`) and a racer's
place on it is the game's own sum (`bottom_map_point`, after
`minimap_marker_pos`). The portraits are the menus' textures, loaded for a
moment and copied out once per level, at the same frame on every console of
a session. Characters are numbered as in `enum Character`
(`include/enums.h`): Krunch, Bumper, Tiptup, Conker, Timber, Banjo,
Drumstick, Pipsy, T.T., Diddy. The names of the `gMenuPortrait*` variables
in `src/menu.c` are in another order and are wrong about which picture they
hold; the first version here took the names' order from them and called
Diddy "Timber".

Cost. It is drawn by the game's thread at the controller poll, in software,
so it draws as little as it can: everything goes into a copy of the screen
and only the part that changed is copied to the frame buffer and flushed;
the map box (a ready-made picture plus eight dots) two polls in three, the
standings only while a row moves and then every second poll, the numbers
when they change. The background is a still picture for the same reason. In
the emulator at the Old 3DS clock a race costs no more frame time than
before (`native/tests/bottom.txt` on both builds); a console has not been
measured.

Pictures. The logo, the banana and the background are not in the source
tree. The builder (`builder/picture.py`) makes `bottom/logo.bin` and
`banana.bin` from the ROM's own textures (the title screen's logo, the race
display's banana) and draws `sky.bin` itself, and puts the three in
`output/sdcard/3ds/DKR/bottom/`, which goes to `sdmc:/3ds/DKR/bottom/` with
the rest. Without them the pages show the title and the word BANANAS as text
on plain dark blue. (`native/tests/sky.txt`, with the test command
`HIDETEXT`, takes a picture of the multiplayer menus' sky without their
words.)

Tests: `bottom` (title screen and a Tracks mode race), `bottom-adventure`
(the island), `bottom-mp-host` with `bottom-mp-join` (a session, with
`tools/run-localplay.sh`); each takes pictures of the touch screen
(`out/native-run/bottom.png`, `out/localplay-run/*-bottom-*.png`).

## Widescreen

`main.c` shows N64 x from -40 to 360. The world is drawn wider; the 2D layer
keeps its size in the middle 320 columns; full-width rectangles are widened.
Flat geometry that must cover the picture (the screen transitions) is
wrapped in `PC_STRETCH_MARK` (`include/f3ddkr.h`, used in
`src/fade_transition.c`) and stretched to the full width.

A view that is a window in a menu — the track's picture in its wooden frame,
on the track select and after a race — is not moved or widened, because its
frame is 2D and stays put. `apply_viewport` and `apply_clip_rect` in `main.c`
decide: a side of a view that lies on the edge of the 320 columns goes out to
the edge of the screen, any other side stays where the game put it, and the
world inside is drawn 1.25x as wide about the view's own centre.
`native/tests/picture.txt` shows the picture still, sliding between tracks,
growing to fill the screen and in the menu after a race.

Tiled backgrounds. The game fills two kinds of menu background with a
repeating tile, both laid out for 320 columns; both now carry the pattern on
to the screen's edges instead of leaving bars (repeated, not pulled wider):

| Background | Drawn by | How it reaches the edges |
|---|---|---|
| Track select (the strips behind the track's picture) | `func_8008F618` in `src/menu.c` | each strip takes `TRACK_BG_EXTRA` (40) more of its texture on either side, and `PC_STRETCH_MARK` lets it be drawn out there |
| Results, race order, trophy rankings (the "mosaic") | `bgdraw_texture` in `src/rcp_dkr.c` | the rows of tiles are laid out 400 wide; between `PC_STRETCH_MARK`s a textured rectangle's column 0 is the screen's left edge (`handle_texrect` in `main.c`) |

Those are the only tiled backgrounds in the game's code (the other loops
over the screen's width are unused: `bgdraw_chequer`, `screenimage_draw`).
A new one that stops at 320 gets the same treatment as whichever of the two
it is drawn like.

## Multiplayer

The title screen has a third entry, MULTIPLAYER: LOCAL WIRELESS or ONLINE,
then HOST A GAME or JOIN A GAME (`localplay_menu.inc` draws it with the
game's fonts; `netplay.c` runs it).

- Local wireless: the host creates a network, the others see it in a list.
- Online: there is no server. The host's console is the other end, and the
  game's code (twelve characters, `code_from_address` in `netplay.c`) is the
  host's public address and port. The host's router must let the port in:
  `upnp.c` asks it to and removes the forwarding afterwards; with UPnP off
  the port (UDP 6464) has to be forwarded by hand. `tools/upnp-port.py`
  shows or removes a forwarding left behind.

Either way everybody sees the player list and the host starts. From then on
every console runs the same game from the same seed, save and controller
samples, and they go to the character select together. A sample is used a
few polls after it is read (three at least, more for a slow connection: the
lobby measures the round trips), which is the time it has to reach everyone.

What differs between consoles is only what is shown and heard:

- each console draws its own player's view on the whole screen
  (`PC_VIEW_MARK`, `view_*` in `main.c`), so there is no split screen, and
  hears from its own player's camera (`audspat_update_all`);
- at the character select each console shows its own player's sign and
  plays its own player's sounds (`localplay_shows_player`), and two players
  may pick the same character;
- rankings carry "P1".."P4" tags, and the touch screen says which player
  the console is (and, for an online host, the code).

What is the same on every console but different from the game's split
screen, since each view has a screen to itself:

- the picture is the single-player one: `cam_get_viewport_layout` answers
  "one view", which brings back the sky dome, weather, shadows, scenery and
  the single-player cameras; the HUD is the single-player HUD (`hud_init`);
- a race goes on until every player has finished, or 45 seconds after the
  first racer of any kind has (`pc_session_race_is_over` in
  `src/objects.c`); on one television it stops when one racer is left;
- a player who leaves the pad alone in a race for 30 seconds is named on the
  other consoles' touch screens, and after a minute is taken out of the
  session (`pc_session_watch_idle`, `netplay_drop_player`); the race goes on
  for the others. The host cannot be dropped and the session kept;
- the rankings table has a row for every racer and counts each player's
  real finishing position (`gLocalLastPlace`), computer racers included;
- after the rankings the game goes straight to the track select. The menus
  between races are one player's to work, the host first and then each
  race's winner (`localplay_chooser`; `menu_input` reads that player's pad
  as the first controller): track, with L for a random one, vehicle, number
  of racers. Only the host can pause;
- computer racers fill the grid by default, and every track and character
  is open: a session uses the complete save that is built in, and the
  console's own save is put back when the session ends.

Points, after Mario Kart Wii's VR as Retro Rewind plays it
(`netplay_report_race` and the comment above it in `netplay.c`; the code
says "rating"). Everybody starts at 0 and the points can go below it. After
each race every pair of players settles up: the one who came in ahead wins
points, the other loses some. The stake is 16 between two players with the
same points, more when the winner had fewer points than the loser (up to
48) and less when the winner had more (down to 2). Then each side's own
points count: the winner is paid the whole stake at 0 or below and less the
higher they stand (half from 4000); the loser pays half the stake at 0, a
quarter at -1000 and below, all of it from 2000. So a player low down loses
little and gains much, and one high up gains little and loses much.

| Race | Result |
|---|---|
| Two players at 0 | winner +16, loser -8 |
| Four players at 0 | +48, +24, 0, -24 |
| A player at 0 beats one at 2000 | +48, and the other -48 |
| A player at 2000 beats one at 0 | +1, and the other -1 |

Computer racers pay and take nothing. Every console works out the same
numbers for everybody. Online the points are the player's own, kept with
the name in `profile.txt` (`points`; a file from before, with `rating`,
starts again at 0); there being no server, nothing stops a player editing
the file. A local wireless session has its own points, from 0, which end
with it. The touch screen shows them beside each player in a race, and the
rankings screen shows them with what the race gave or took.

How a session ends. Everybody goes back to the title screen together when
the leader backs out of the character select (B on the track screen, then B
again), and `menu_title_screen_init` closes the session on each console
(`localplay_session_close`). A console also goes there by itself when the
others are gone (`netplay_take_lost` in `src/thread3_main.c`). The two meet:
the consoles are a poll or two apart in real time, so when the leader backs
out, whichever console is behind hears the other's goodbye before its own
menu gets to the title screen, and leaves by the second way, from inside a
menu. That way must unload the menu and the scene behind it first
(`localplay_menu_abandon`, then `unload_level_menu`), as leaving a race
unloads the race. It did not, the title screen's scene was loaded on top of
the character select's, and the old scene's cameras went on moving the
picture: the title screen came up with the camera under the sea or inside a
wheel instead of Diddy and his friends coming out of the bushes.
`native/tests/lp-host-backout.txt` with `lp-join-backout.txt` plays a race
to its end and backs out; both consoles' last pictures must be the title
screen's own scene (which console is the one behind changes from run to
run). `native/tests/title-return.txt` shows the same scene on one console,
at first start and after a race, to compare with.

Test with two emulators: `tools/make-lp-tests.py` writes the scripts,
`tools/run-localplay.sh native/tests/on-host.txt native/tests/on-join.txt 300`
runs them (`lp-*` for local wireless) and prints whether the two consoles'
state checks agree. The script key FINISH ends a race for a player. Other
pairs of scripts in `native/tests/`: `late-*` (computer racers finish first),
`idle-*` (a player is dropped), `win-*` (the winner picks a random next
track; ratings), `rand-host` (the host does).

### Changing the multiplayer menus by hand

Every multiplayer screen is text over the sky background, and all of it is in
one file, `3ds/multiplayer_menu.inc`: the lobby (MULTIPLAYER, host or join,
PLAYERS) and, in a session, CHOOSE A TRACK and CHOOSE A VEHICLE, which
replace the game's own track select there (`menu_init` in `src/menu.c` sends
a session to `MENU_SESSION_TRACKS`). The first part of the file is tables;
the second part, how the screens behave, should not need touching for
wording or layout. The screen is 320 wide and 240 high, y counts down from
the top, text is centred. After a change: build, then
`tools/run-localplay.sh native/tests/mp-menu-host.txt native/tests/mp-menu-join.txt`
and look at `out/localplay-run/host.png`.

Three examples:

- **Rename an option.** In `sMpText`, change the words between the quotes:
  `[T_HOST] = { 104, "HOST A GAME" },` to `[T_HOST] = { 104, "OPEN A ROOM" },`.
  Lines with `%s` or `%d` in them get a name or number put in by the program;
  keep those marks, in the same order.
- **Move a line.** The number before the words is its height on the screen:
  `[T_RANDOM] = { 176, "RANDOM" },` to `{ 170, "RANDOM" }` moves it up six.
  The lists' first row and spacing are the `MP_*_ROW_Y` and `MP_*_ROW_STEP`
  numbers at the top.
- **Add or remove a track.** `sMpTracks` has one line per track: the world it
  is listed under, the name shown, the game's level
  (`ASSET_LEVEL_*` in `include/asset_enums.h`). Tracks of one world must be
  next to each other; each world is a page. Deleting a line removes the
  track from multiplayer; which vehicles it allows is the game's own
  knowledge and is not written there.

More players between races: LET MORE PLAYERS JOIN on the track screen sends
every console back to the PLAYERS lobby at the same poll, with the connection
kept and open to new consoles (`netplay_back_to_lobby`,
`link_host_open_doors`); the host starts again from there
(`native/tests/relobby-*.txt`, three consoles: `PLAYERS=3
JOIN3=native/tests/relobby-late.txt tools/run-localplay.sh
native/tests/relobby-host.txt native/tests/relobby-join.txt`). The menu's
music is held on while these screens show and let go when the race starts
(`mp_start_race`).

## Small things added to the game's own screens

- The opening logos (the turning N64 and the Rareware logo, 16 seconds): any
  button ends them with a short fade, and the title screen comes up with its
  logo and options at once instead of holding them back for the opening
  scene. `menu_logo_screen_loop` in `src/menu.c`. A test script's keys do not
  do this unless the script says `SKIPLOGOS` first, so the scripts written
  before keep their timing (`native/tests/logoskip.txt`).

- Track select (Tracks mode, alone or in a session; not Adventure): a RANDOM
  option under the track's name, the menu's arrow turning beside it, taken
  with L. `trackmenu_input` in `src/menu.c`.
- Character select: if there are more columns of characters than the screen
  holds (`gCharSelectVisibleColumns`, five), the view slides sideways one
  column at a time to keep the cursor on screen. The columns are worked out
  from where the characters' objects stand, so a mod that adds characters
  needs to change nothing here. `charselect_pan` in `src/menu.c`,
  `gViewShiftX/Z` in `src/camera.c`. The test script command `COLUMNS n`
  pretends the screen is narrower (`native/tests/charselect.txt`).

## Added characters

Character patches for the N64 game (`.xdelta`) each replace one of the ten
racers. Here they become extra characters beside the ten.

The release comes with 26 of them, which the builder adds by itself from
`characters/added-characters.pack` (`docs/BUILDING.md`, "The added
characters' pack"). What follows is how such a set is made.

**Adding some.** Put the `.xdelta` files in a folder and run, from the
project root:

```sh
python tools/import-characters.py --rom rom/your-rom.z64 --patches characters
```

To add patches to an existing generated roster, pass its folder as
`--base-characters out/characters`; the existing entries are retained.

It writes `out/characters/`: `assets.bin` and `assets.lut.bin` (the game's
assets with the characters' models, animations, textures, portraits and
sound bank entries appended as new records), the folder `voices` (a file of
voice samples per character), `characters.txt` (the list) and
`portraits.png` to look at. Copy all but the picture to `sdmc:/3ds/DKR/`,
and delete a `voices.bin` there: it belonged to packs made before each
character had its own file. A `names.txt` in the patch folder names
characters or leaves them out; the tool's own top comment has the format and
the asset file's layout. `tools/vcdiff.py` is the patch decoder (xdelta3's
LZMA variety, which ready-made Python decoders do not read).

What a character is made of, and where the game uses it:

| | From the patch | Stays its donor's |
|---|---|---|
| Models (car, hover, plane), animations, textures | yes: `Racer.unk7` of the settings picks its objects when racers spawn (`src/objects.c`) | |
| Figure on the character select | yes: spawned by `charselect_spawn_added` (`src/menu.c`), run by `obj_loop_char_select` (actorIndex 100 and up) | |
| Portrait in results | yes: `racer_portrait` (`src/menu.c`) | |
| Portrait in the challenges (over the banana count of a battle, the eggs, the treasure) and of the second player in the hub | yes: `hud_portrait_sprite` (`src/game_ui.c`) | |
| Sign over its nest or chest in a challenge | yes: `hud_added_portrait_texture`, worn by `obj_loop_characterflag` (`src/object_functions.c`) | |

Those signs were broken for every character, the game's ten included: half a
picture at one nest and none at the next. The game puts a triangle's first
word (flags and three corners) and its texture coordinates together with
shifts, the N64's way round; `DKR_TRIANGLE` and `DKR_TEXCOORDS`
(`include/structs.h`) now write them for this processor. The rain's two
triangles and the eight-sided fan of `src/objects.c` are built the same way
and are corrected with them. `native/tests/battle.txt` runs a challenge.
| Voice lines | yes: `RACER_SOUND` in `src/racer.c`, `CURSOR_SOUND` in `src/menu.c`, both `modchar_sound`; samples read from the character's own `voices/<character>.bin` on demand (`voice_want` in `characters.c`, served to the audio thread by `pc_dmacopy`) | engine sound; the horn, unless the patch has one |
| Handling, weight, speed, music channel on the select | | yes: `Racer.character` is the donor's number, so the game's own tables apply |

**Voices: each character its own.** A character has 19 lines: two on the
character select ("I'm Diddy", and the one when un-chosen), the horn, eight
cheers and eight groans. The game's ten have theirs in the game's sound
bank, one sound id per character and line (`sound_voice_line` in
`src/audio.c` tells whose line an id is). An added character's are
recordings from its patch, each a new sound id, kept in a voice file of that
character alone. One function decides what is played, `modchar_sound` in
`characters.c`, by three rules:

1. One of the game's ten plays the game's own sound, untouched.
2. An added character plays only out of its own voice file. A recording's
   address in the sound bank holds the number of the character it belongs
   to and the place in that character's file; before it is read it must
   belong to the character speaking and be where the file's own list says
   (`voice_want`). Otherwise there is silence and a line in the log
   (`CHARACTERS: ... does not play sound ...`), never another's voice.
3. A line the patch has no recording for is never the donor's voice. For a
   cheer or groan the character's own nearest one is played; a character
   select line stays silent. The horn alone falls back to the donor's: it is
   a car horn, not a voice, and most patches bring none.

The importer gives a character only the recordings of its own lines (a
patch that also changes other characters' lines or effects has those left
alone), reads back what it wrote and compares it with what it meant to
write, and refuses to call a pack good otherwise.
`python tools/check-voices.py` does that for a pack at any time, against the
patches and the ROM, and prints who says what; `--wav out/voices-wav` also
writes every line of every character, the game's ten included, as a `.wav`
in a folder per character, to listen to on the PC. The game checks itself
with the test command `VOICECHECK` (`native/tests/voicecheck.txt`): it
fetches every recording of every added character as a race would and logs a
check sum per character, which `check-voices.py --log out/native-run/log.txt`
compares with the pack.

That the lines are also heard is checked by `tools/play-voices.sh` (about
25 minutes, `DKR_BUILD=...` as for `run-native.sh`): every line of the ten
and of every added character, 640 with this pack, is played with `SOUND` on
a silent screen while the game records what it mixes, and
`tools/play-voices.py check` looks for each line's own recording in that
(the wave forms must agree; an unrelated recording scores about 0.1 where
the right one scores 0.9 to 1.0). `out/play-voices/NN.report.txt` has a row
per line. Two things it taught, both the game's own doing and the same on
an N64:

- A line is played at the sound table's pitch times its key map's
  (`keyBase` 48, half speed, for most of the game's voices).
- A sound's envelope ends it after its attack and decay times however long
  the recording is. Two of the game's lines (Tiptup, Timber) and eleven of
  Link's, Yoshi's and Red Yoshi's are shorter than their recordings for
  that reason: those patches put longer recordings under the envelopes of
  the lines they replaced.

Why this is so strict: until 2026-10-08 all voices were in one `voices.bin`,
found through sample offsets that depend on where the sound bank lies in
`assets.bin`. Adding three characters to the existing pack moved the sound
bank by 397,072 bytes and left the 23 earlier characters' offsets as they
were, so each of their lines played whatever lay that much further on in
the file: another line, usually another character. The three new ones had
their rows in the sound table two bytes out of place and played nothing or
anything. Nothing in the game or the tools noticed. Now the importer writes
the whole sound bank afresh every time (also with `--base-characters`), and
both it and the game check each recording's owner and place.

**Unlocking.** Every banana this console's player picks up in any race goes
into a bank (`bananas.txt`: the bananas, and `purchased`, one bit per
character of the list). STORE on the title screen lists the added
characters; one costs 150 bananas (`MODCHAR_BANANAS_EACH`). The characters
that can be chosen are exactly the ones bought, whichever they are:
`modchar_available_number` gives the n-th of them, and the character select,
its voices and the computer racers all go through it. (Counting "the first
so many of the list" instead is how a bought Shovel Knight once appeared as
Bomberman.) In a session between consoles every character of the list is
available, provided all consoles have the same list (the `roster` number,
carried in the lobby's packets); if they differ the session has none,
because every console must run the same race.

**Character select.** `charselect_add_characters` builds the table of
cursor moves at run time (the game has fixed ones for 8, 9 and 10 figures):
the added figures stand in further columns to the right, the view slides to
them (`charselect_pan`), their name is shown under the title, and the
stage's scenery is drawn again beside itself so that they do not stand over
nothing (`pc_draw_stage_copies` in `src/tracks.c`).

Cost: a figure is about 500 vertices and 1 ms of the interpreter at the Old
3DS clock, so only what the screen shows is drawn: figures off its sides are
skipped in `render_level_geometry_and_objects` (`cam_point_across_screen`),
copies of the stage out of view in `pc_draw_stage_copies`, and the added
columns stand wider apart (`CHARSELECT_ADDED_SPACING`). With all 23 on the
stage that took the screen from 34 ms a frame to 11-18 ms in the emulator
(the plain select is 9 ms); the patches' models are what is left.

**Computer racers** take added characters by chance, in proportion to how
many are available (`charselect_assign_added_ai`).

Tests: `voicecheck`, `addedchars`, `addedrace`, `addedfar` (run with
`DKR_CHARACTERS=out/characters DKR_BANANAS=11500 tools/run-native.sh ...`)
and `lp-join-added` for a session (`DKR_CHARACTERS=out/characters
tools/run-localplay.sh native/tests/lp-host.txt native/tests/lp-join-added.txt`).

## Checking sound without ears

`DKR_AUDIODUMP=1 tools/run-native.sh <test>` makes the game record
everything it mixes (`audio.raw`, also when an empty `AUDIODUMP.TXT` is in
the game's folder); `python tools/audio-check.py` turns that into
`audio.wav` and a loudness bar per stretch of time. Test commands for it:
`NOMUSIC` (music volume to nothing), `SOUND n` (play sound id n),
`BANKSOUND n` (play recording n). `native/tests/sounds.txt` plays an added
character's voice, a looping engine recording and an effect on a silent
screen; `race-sfx.txt` is a race without the music.

What that found: the added characters' voices were silent because
`voices.bin` was blank for every character but the last three. The
importer's `--base-characters` mode (adding patches to an existing pack)
started the new voice file as so many zeros instead of the old file's bytes.
Putting the bytes back made them audible but not right; see "Voices: each
character its own" above for that and for the checks that replaced
listening. A looping engine recording played by itself sustains as it
should.

## Hard mode

A switch on the touch screen menu (Game, "Hard mode"; `hard_mode` in
`settings.ini`). The computer racers of an ordinary race are held to a
schedule taken from real times: the middle between T.T.'s ghost for the
track (from the game's own ghost data) and the fastest three laps on
speedrun.com's level leaderboards. The table, the sources and the knobs are
in `hardmode.c`; `pc_hard_mode_bananas` in `src/racer.c` works out a racer's
progress and both of the game's ways of moving a computer racer take the
result (`handle_racer_top_speed`, and the routine that runs them along the
track's line while no player is near). It only ever raises the game's own
speed figure and keeps the game's limit, so it is off in effect wherever a
track is not in the table. Not in sessions between consoles. The log has a
line per computer racer at the finish (`HARD:` with its schedule, `RACE:`
without hard mode). Ancient Lake in the emulator: 80-84 s without, 60-64 s
with, which is the limit and about T.T.'s time; the experts' 45.5 s is out
of the computer racers' reach.

## Saves from the PC version

`python tools/import-save.py` copies the adventure games of the newest DKR
Recompiled save (`%APPDATA%/DKRPort/saves`) into free places of the 3DS save
on the SD card (GAME A, B, C), leaving what is there and keeping a copy
(`eeprom.bin.before-import`). The two are the same 512 bytes of cartridge
EEPROM; nothing in the program is involved.

## Switches

Empty files in `sdmc:/3ds/DKR/`:

| File | Effect |
|---|---|
| `NO_RENDER_THREAD.TXT` | Interpret and draw on the game thread |
| `AUTOTEST.TXT` | Scripted input, touches and screenshots; format at the top of `autotest.c` |
| `settings.ini` | Written by the touch screen menu; delete it for the defaults |
| `eeprom.bin`, `eeprom-unlocked.bin` | The two save files (512 bytes, the N64 cartridge's EEPROM). The touch screen menu's "Save file" row picks which the game uses from its next start; the second begins as the complete save built into the program and also unlocks Drumstick and T.T. (`pc_save_is_unlocked`) |
| `characters.txt`, `voices/` | The added characters' list and their voice files, one per character, with the `assets.bin` that `tools/import-characters.py` wrote beside them; all of one pack, or the characters are silent. Without `characters.txt` the game is the plain one |
| `bananas.txt` | The banana bank: `bananas 1234` |
| `bottom/logo.bin`, `bottom/banana.bin`, `bottom/sky.bin` | The touch screen's pictures (the builder makes them); text and a plain background without them |
| `lan.txt` | Local wireless stood in for by UDP on the access point; format at the top of `link.c` |
| `online.txt` | Online play: the port, an address to put in the code instead of asking the router, `noupnp`, a `code` to join without the keyboard; format in `link.c` |
| `NETLOG.TXT` | Local Play logs every poll's state check, to find where two consoles part ways |

## Rules that keep it working

- Nothing in the interpreter may run per vertex what can run per material,
  nor per triangle what can run per batch. `shade_model_prepare`,
  `projected_vertex` and the batch in `gfx.c` exist for that.
- Nothing between `gfx_frame_begin` and `gfx_frame_end` may send the GPU a
  command or free what it reads: it is still drawing the frame before. State
  goes into `sState`, draws into records, and a texture that is deleted
  lingers for two hand-overs.
- Nothing that happens every frame or every second may write to the SD card
  on the game thread: a write takes a console longer than a frame. The log
  goes through its buffer for that reason.
- citro3d is only called from the render thread once it has started.
- The game thread must not read or write what the render thread owns
  (`main.c`'s statics). Memory the game frees is reported through
  `pc_gfx_invalidate_range`, which only queues the range.
- In a multiplayer session nothing may differ between consoles except the
  controllers: no wall-clock time, no sound-card state, in anything the race
  depends on. Presentation-only randomness goes through `gPresentationRandom`
  (`src/hasm/math_util.c`).

### Drawing and the race

Each console builds every player's scene and the interpreter draws only its
own (`SESSION_DRAW_OWN_VIEW_ONLY` in `src/tracks.c` is 0). Building only the
console's own would be faster, but drawing is not free of side effects on
the race, and two consoles that draw different things drift apart. What was
found and fixed, each with a comment where it is:

- computer racers nobody had drawn used a cheaper driving model
  (`racer->unk201`): always the full one in a session;
- particles drew on the game's random numbers: their own now
  (`src/particles.c`);
- a racer moved for the picture was put back by subtraction, not exactly
  (`sDrawSaved*` in `src/objects.c`);
- the object list was left in back-to-front drawing order, which is also
  the order objects are updated in (`pc_session_object_order_*`).

Still open: an animated solid object only gets the shape racers collide
with when it is drawn (`render_3d_model`). To find the next one: run a
session with `netlog` (`tools/run-localplay.sh ... netlog`), then
`python tools/netlog-diff.py`, which names the first random-number call,
state-check part, object and racer field that differ between two consoles.

### Eight players: where it stands

Sessions are capped at four players (`NETPLAY_MAX_PLAYERS`), because the
game has four of everything a human racer needs. What a slot consists of,
and its state:

| Piece | Where | State |
|---|---|---|
| A grid of eight racers, computer racers in the empty seats | `src/objects.c` (`gNumRacers = 8` in a session) | done |
| Cameras: one per player and one cutscene camera each | `PLAYER_SLOTS`, `CUTSCENE_CAMERA_OFFSET` in `src/camera.h`; `gCameras`, `gScreenViewports` | widened to eight |
| Tests with any number of consoles | `PLAYERS=n tools/run-localplay.sh` | done; two, three and four pass |
| Controllers | `MAXCONTROLLERS` (`include/PR/os_cont.h`), `src/joypad.c`, `PC_MAXCONTROLLERS` in `reimpl.c`, `PLAYER_MENU` (the "any controller" index, `src/menu.h`) | four |
| View layouts | `cam_set_layout` knows one to four views; the scene loop in `src/tracks.c` runs over them | four |
| Racer set-up | `src/objects.c` gives `playerIndex` 0-3 to the first racers | four |
| HUD data | `gPlayerHud[MAXCONTROLLERS]`, `hud_init` | four |
| Everything else indexed by `playerIndex` | about 270 uses across `src/` (rumble, sounds, magnets, weapons...) | to audit: a build with `-fsanitize=bounds` and a five-console test will find the four-long arrays |
| Menus | character select (`gActivePlayersArray[4]`, the signs), vehicle select, rankings columns | four |
| Lobby and link | `NETPLAY_MAX_PLAYERS`, `LINK_MAX_NODES`, the player list on screen | four |

The order to do them in is the table's: each row can be tested on its own
with four consoles before the cap is raised.
