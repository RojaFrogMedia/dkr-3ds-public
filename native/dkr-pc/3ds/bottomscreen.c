// The touch screen while the settings menu is closed (bottom.h):
//
//   in a race      the standings (place, portrait, name; the rows slide up
//                  and down as places change), the track's map with every
//                  racer on it, speed, lap, bananas; between consoles the
//                  players' names and points
//   Adventure hub  what the save still has to collect
//   anywhere else  the logo and the banana bank
//
// To move or resize something, change the numbers under "layout". Everything
// is drawn into a copy of the screen first and only the part that changed is
// copied to the real one, so nothing is ever seen half drawn; and only what
// changed is drawn at all (the map twice in three polls, the rest when a
// value changes), because this runs on the game's own thread.
//
// The logo, the banana and the background (a still sky) are pictures from
// the game's folder (bottom/logo.bin, banana.bin, sky.bin, made by the
// builder: builder/picture.py); without them the same places hold text and
// a plain dark blue.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bottom.h"
#include "characters.h"
#include "netplay.h"

extern const uint8_t default_font_bin[];    // libctru's console font: 256 glyphs of 8 rows

#define SCREEN_W 320
#define SCREEN_H 240
#define RGB(r, g, b) (((uint32_t) (r) << 16) | ((uint32_t) (g) << 8) | (uint32_t) (b))

// ---- layout (pixels; y counts down from the top)

#define TOP 18                  // the rows above belong to touchmenu.c's header

#define LIST_X 2                // the standings
#define LIST_Y 21
#define LIST_W 148
#define ROW_STEP 27             // from one row to the next
#define ROW_H 25
#define PLACE_X 4               // within a row
#define PORTRAIT_X 22
#define PORTRAIT_SIZE 24
#define NAME_X 50

#define PANEL_X 152             // everything right of the standings
#define PANEL_W 166
#define MAP_Y 21
#define MAP_H 122
#define MAP_MARGIN 5
#define MAP_SCALE_MAX 3         // times the game's own map
#define INFO_Y 146              // speed and lap
#define INFO_H 44
#define COUNT_Y 192             // bananas, silver coins
#define COUNT_H 24
#define FOOT_Y 218              // collectables, or a warning

#define LOGO_Y 24               // the menu page
#define BANK_Y 160
#define HINT_Y 224

#define COL_BACK    RGB(14, 22, 52)
#define COL_PANEL   RGB(24, 36, 80)
#define COL_ROW     RGB(30, 44, 96)
#define COL_ROW_ME  RGB(150, 104, 16)
#define COL_EDGE_ME RGB(255, 214, 64)
#define COL_TEXT    RGB(255, 255, 255)
#define COL_DIM     RGB(150, 165, 200)
#define COL_GOLD    RGB(255, 214, 64)
#define COL_GREEN   RGB(110, 230, 120)
#define COL_RED     RGB(196, 40, 40)
#define COL_DARK    RGB(8, 12, 28)
#define COL_BAR     RGB(50, 66, 120)

// ---- drawing: into a copy of the screen, laid out like the frame buffer
// (columns first, bottom row first, blue-green-red)

static uint8_t sCopy[SCREEN_W * SCREEN_H * 3];
static uint8_t sBack[SCREEN_W * SCREEN_H * 3];     // the background, in the same layout
static int sDirtyX0, sDirtyY0, sDirtyX1, sDirtyY1;     // the part not yet on the screen; x1 <= x0: none

static void dirty(int x, int y, int w, int h) {
    if (sDirtyX1 <= sDirtyX0) {
        sDirtyX0 = x; sDirtyY0 = y; sDirtyX1 = x + w; sDirtyY1 = y + h;
        return;
    }
    if (x < sDirtyX0) sDirtyX0 = x;
    if (y < sDirtyY0) sDirtyY0 = y;
    if (x + w > sDirtyX1) sDirtyX1 = x + w;
    if (y + h > sDirtyY1) sDirtyY1 = y + h;
}

static inline void put(int x, int y, uint32_t colour) {
    if (x >= 0 && x < SCREEN_W && y >= TOP && y < SCREEN_H) {
        uint8_t *p = sCopy + (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;

        p[0] = (uint8_t) colour;
        p[1] = (uint8_t) (colour >> 8);
        p[2] = (uint8_t) (colour >> 16);
    }
}

// `alpha` of 255 parts of `colour` over what is there.
static inline void blend(int x, int y, uint32_t colour, unsigned alpha) {
    if (alpha >= 250) {
        put(x, y, colour);
    } else if (alpha > 4 && x >= 0 && x < SCREEN_W && y >= TOP && y < SCREEN_H) {
        uint8_t *p = sCopy + (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;

        p[0] = (uint8_t) (p[0] + ((int) (colour & 255) - p[0]) * (int) alpha / 255);
        p[1] = (uint8_t) (p[1] + ((int) ((colour >> 8) & 255) - p[1]) * (int) alpha / 255);
        p[2] = (uint8_t) (p[2] + ((int) ((colour >> 16) & 255) - p[2]) * (int) alpha / 255);
    }
}

static void fill(int x, int y, int w, int h, uint32_t colour) {
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w, y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    int ix, n;

    x = x < 0 ? 0 : x;
    y = y < TOP ? TOP : y;
    if (x1 <= x || y1 <= y) {
        return;
    }
    for (ix = x; ix < x1; ix++) {
        uint8_t *p = sCopy + (ix * SCREEN_H + (SCREEN_H - y1)) * 3;

        for (n = y1 - y; n > 0; n--, p += 3) {
            p[0] = (uint8_t) colour;
            p[1] = (uint8_t) (colour >> 8);
            p[2] = (uint8_t) (colour >> 16);
        }
    }
    dirty(x, y, x1 - x, y1 - y);
}

// The background put back over a part of the screen.
static void clear(int x, int y, int w, int h) {
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w, y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    int ix;

    x = x < 0 ? 0 : x;
    y = y < TOP ? TOP : y;
    if (x1 <= x || y1 <= y) {
        return;
    }
    for (ix = x; ix < x1; ix++) {
        size_t at = ((size_t) ix * SCREEN_H + (size_t) (SCREEN_H - y1)) * 3;

        memcpy(sCopy + at, sBack + at, (size_t) (y1 - y) * 3);
    }
    dirty(x, y, x1 - x, y1 - y);
}

// A part of the screen darkened to `keep` of 256, for text to stand on.
static void shade(int x, int y, int w, int h, int keep) {
    int ix, n;

    for (ix = x; ix < x + w && ix < SCREEN_W; ix++) {
        uint8_t *p = sCopy + (ix * SCREEN_H + (SCREEN_H - (y + h))) * 3;

        for (n = h * 3; n > 0; n--, p++) {
            *p = (uint8_t) (*p * keep >> 8);
        }
    }
    dirty(x, y, w, h);
}

static void frame(int x, int y, int w, int h, uint32_t colour) {
    fill(x, y, w, 1, colour);
    fill(x, y + h - 1, w, 1, colour);
    fill(x, y, 1, h, colour);
    fill(x + w - 1, y, 1, h, colour);
}

// Text at a pixel position; scale 2 doubles the glyphs.
static void text(int x, int y, const char *string, uint32_t colour, int scale) {
    int startX = x, sx, sy, gx, gy;

    for (; *string != '\0'; string++, x += 8 * scale) {
        const uint8_t *glyph = default_font_bin + (uint8_t) *string * 8;

        for (gy = 0; gy < 8; gy++) {
            for (gx = 0; gx < 8; gx++) {
                if (glyph[gy] & (0x80 >> gx)) {
                    for (sx = 0; sx < scale; sx++) {
                        for (sy = 0; sy < scale; sy++) {
                            put(x + gx * scale + sx, y + gy * scale + sy, colour);
                        }
                    }
                }
            }
        }
    }
    dirty(startX, y, x - startX, 8 * scale);
}

static int text_width(const char *string, int scale) {
    return (int) strlen(string) * 8 * scale;
}

static void text_centred(int x, int w, int y, const char *string, uint32_t colour, int scale) {
    text(x + (w - text_width(string, scale)) / 2, y, string, colour, scale);
}

// Text with a dark edge, for over pictures and bright rows.
static void text_edged(int x, int y, const char *string, uint32_t colour, int scale) {
    text(x + 1, y + 1, string, COL_DARK, scale);
    text(x, y, string, colour, scale);
}

// ---- pictures: 0xAARRGGBB, rows from the top

typedef struct {
    int w, h;
    uint32_t *pixels;       // NULL: there is none
} Image;

static void image_draw(const Image *image, int x, int y) {
    int ix, iy;

    if (image->pixels == NULL) {
        return;
    }
    for (iy = 0; iy < image->h; iy++) {
        for (ix = 0; ix < image->w; ix++) {
            uint32_t pixel = image->pixels[iy * image->w + ix];

            blend(x + ix, y + iy, pixel & 0xFFFFFF, pixel >> 24);
        }
    }
    dirty(x, y, image->w, image->h);
}

// A picture made smaller or larger: each new pixel is the average of the
// part of the old picture it covers (colours weighted by how solid they are).
static void image_scale(const void *sourcePixels, int sw, int sh, Image *out, int w, int h) {
    const uint32_t *source = sourcePixels;      // `unsigned` in bottom.h, the same 32 bits
    int x, y, sx, sy;

    if (out->pixels == NULL || out->w != w || out->h != h) {
        free(out->pixels);
        out->pixels = malloc((size_t) w * h * sizeof(uint32_t));
        out->w = w;
        out->h = h;
    }
    if (out->pixels == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        int y0 = y * sh / h, y1 = (y + 1) * sh / h;

        y1 = y1 > y0 ? y1 : y0 + 1;
        for (x = 0; x < w; x++) {
            int x0 = x * sw / w, x1 = (x + 1) * sw / w;
            unsigned a = 0, r = 0, g = 0, b = 0, n = 0;

            x1 = x1 > x0 ? x1 : x0 + 1;
            for (sy = y0; sy < y1 && sy < sh; sy++) {
                for (sx = x0; sx < x1 && sx < sw; sx++) {
                    uint32_t pixel = source[sy * sw + sx];
                    unsigned alpha = pixel >> 24;

                    a += alpha;
                    r += ((pixel >> 16) & 255) * alpha;
                    g += ((pixel >> 8) & 255) * alpha;
                    b += (pixel & 255) * alpha;
                    n++;
                }
            }
            out->pixels[y * w + x] = (a == 0 || n == 0) ? 0 : (((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a));
        }
    }
}

// A .bin of the builder (builder/picture.py): "DKRI", width, height, then RGBA.
static void image_load(const char *path, Image *out) {
    uint8_t header[8];
    FILE *f = fopen(path, "rb");
    int i, count;

    out->pixels = NULL;
    if (f == NULL) {
        return;
    }
    if (fread(header, 1, 8, f) == 8 && memcmp(header, "DKRI", 4) == 0) {
        out->w = header[4] | (header[5] << 8);
        out->h = header[6] | (header[7] << 8);
        count = out->w * out->h;
        if (out->w >= 1 && out->w <= SCREEN_W && out->h >= 1 && out->h <= SCREEN_H) {
            out->pixels = malloc((size_t) count * sizeof(uint32_t));
        }
        if (out->pixels != NULL) {
            if (fread(out->pixels, 4, (size_t) count, f) != (size_t) count) {
                free(out->pixels);
                out->pixels = NULL;
            } else {
                for (i = 0; i < count; i++) {
                    const uint8_t *p = (const uint8_t *) &out->pixels[i];

                    out->pixels[i] = ((uint32_t) p[3] << 24) | ((uint32_t) p[0] << 16) | ((uint32_t) p[1] << 8) | p[2];
                }
            }
        }
    }
    fclose(f);
}

static Image sLogo;
static Image sBanana;           // as loaded
static Image sBananaBig;        // for the menu page
static Image sBananaSmall;      // for a line of text

// The banana (or the word) and a number after it; returns the width used.
static int banana_count(int x, int y, const Image *banana, int count, int scale) {
    char number[16];
    int width;

    snprintf(number, sizeof(number), "x%d", count);
    if (banana->pixels != NULL) {
        image_draw(banana, x, y);
        width = banana->w + 4;
        text_edged(x + width, y + (banana->h - 8 * scale) / 2, number, COL_TEXT, scale);
        return width + text_width(number, scale);
    }
    text(x, y, "BANANAS", COL_GOLD, 1);
    text(x + 60, y, number, COL_TEXT, 1);
    return 60 + text_width(number, 1);
}

static int banana_count_width(const Image *banana, int count, int scale) {
    char number[16];

    snprintf(number, sizeof(number), "x%d", count);
    return banana->pixels != NULL ? banana->w + 4 + text_width(number, scale) : 60 + text_width(number, 1);
}

// ---- the menu page: the logo and the banana bank

static char sNote[40];          // a line under the bank ("New character: ...")

void bottomscreen_set_note(const char *note) {
    snprintf(sNote, sizeof(sNote), "%s", note);
}

static void draw_menu_page(void) {
    int bank = modchar_bananas();

    clear(0, TOP, SCREEN_W, SCREEN_H - TOP);
    if (sLogo.pixels != NULL) {
        image_draw(&sLogo, (SCREEN_W - sLogo.w) / 2, LOGO_Y);
    } else {
        text_centred(0, SCREEN_W, LOGO_Y + 40, "DIDDY KONG", COL_GOLD, 3);
        text_centred(0, SCREEN_W, LOGO_Y + 70, "RACING", COL_GOLD, 3);
    }
    banana_count((SCREEN_W - banana_count_width(&sBananaBig, bank, 3)) / 2, BANK_Y, &sBananaBig, bank, 3);
    if (sNote[0] != '\0') {
        text_centred(0, SCREEN_W, BANK_Y + 44, sNote, COL_GOLD, 1);
    }
    shade(0, HINT_Y - 4, SCREEN_W, 16, 120);
    text_centred(0, SCREEN_W, HINT_Y, "Touch or press SELECT for settings", COL_TEXT, 1);
}

// ---- the Adventure hub: what is still to be collected

static void collect_row(int y, const char *label, int have, int of) {
    char count[16];
    int barX = 150, barW = 100;
    uint32_t colour = have >= of ? COL_GREEN : COL_TEXT;

    have = have > of ? of : have;
    text(10, y + 6, label, colour, 1);
    fill(barX, y + 5, barW, 10, COL_BAR);
    fill(barX, y + 5, barW * have / of, 10, have >= of ? COL_GREEN : COL_GOLD);
    if (have >= of) {
        snprintf(count, sizeof(count), "ALL");
    } else {
        snprintf(count, sizeof(count), "%d/%d", have, of);
    }
    text(SCREEN_W - 10 - text_width(count, 1), y + 6, count, colour, 1);
}

static void draw_hub_page(const BottomSnapshot *snap) {
    int bank = modchar_bananas();
    int y = 52;

    clear(0, TOP, SCREEN_W, SCREEN_H - TOP);
    shade(4, 21, SCREEN_W - 8, 178, 100);
    shade(4, 202, SCREEN_W - 8, 26, 100);
    text_centred(0, SCREEN_W, 24, "ADVENTURE", COL_GOLD, 2);
    text_centred(0, SCREEN_W, 42, "collected so far", COL_DIM, 1);
    collect_row(y, "GOLDEN BALLOONS", snap->collect.balloons, 47); y += 24;
    collect_row(y, "WIZPIG AMULET", snap->collect.wizpigAmulet, 4); y += 24;
    collect_row(y, "T.T. AMULET", snap->collect.ttAmulet, 4); y += 24;
    collect_row(y, "KEYS", snap->collect.keys, 4); y += 24;
    collect_row(y, "TROPHIES", snap->collect.trophies, 5); y += 24;
    collect_row(y, "BOSS RACES WON", snap->collect.bosses, 10); y += 24;
    banana_count(10, 204, &sBananaSmall, bank, 2);
    text(SCREEN_W - 10 - text_width("SELECT: settings", 1), 210, "SELECT: settings", COL_DIM, 1);
}

// ---- the race page

static int sRowY[BOTTOM_MAX_RACERS];        // where each racer's row is now, in 1/16 pixel
static int sRowCount = -1;                  // racers the rows were set out for
static unsigned sPolls;                     // controller polls, for what is not drawn on every one
static Image sPortraits[BOTTOM_MAX_RACERS];
static unsigned sPortraitSerial[BOTTOM_MAX_RACERS];
static const unsigned *sPortraitSource[BOTTOM_MAX_RACERS];

// The background of the map box with the map on it, made once per track.
// Laid out like the frame buffer (columns first, bottom row first), so that
// putting it on the screen is one copy per column.
static uint8_t sMapPicture[PANEL_W * MAP_H * 3];
static unsigned sMapSerial = ~0u;
static int sMapScale16;                     // screen pixels per map pixel, in sixteenths
static int sMapLeft, sMapTop;               // the map picture's corner on the screen

static void build_map_picture(const BottomSnapshot *snap) {
    int x, y;

    for (x = 0; x < PANEL_W * MAP_H; x++) {
        sMapPicture[x * 3] = (uint8_t) COL_PANEL;
        sMapPicture[x * 3 + 1] = (uint8_t) (COL_PANEL >> 8);
        sMapPicture[x * 3 + 2] = (uint8_t) (COL_PANEL >> 16);
    }
    sMapScale16 = 0;
    if (snap->mapWidth <= 0 || snap->mapHeight <= 0 || snap->map == NULL) {
        return;
    }
    {
        int scaleX = (PANEL_W - 2 * MAP_MARGIN) * 16 / snap->mapWidth;
        int scaleY = (MAP_H - 2 * MAP_MARGIN) * 16 / snap->mapHeight;
        int width, height;
        // The game tints the map with the track's colour; a dark one would
        // not show on this background.
        int r = snap->mapColour[0], g = snap->mapColour[1], b = snap->mapColour[2];

        if (r + g + b < 300) {
            r = g = b = 230;
        }
        sMapScale16 = scaleX < scaleY ? scaleX : scaleY;
        if (sMapScale16 > MAP_SCALE_MAX * 16) {
            sMapScale16 = MAP_SCALE_MAX * 16;
        }
        width = snap->mapWidth * sMapScale16 / 16;
        height = snap->mapHeight * sMapScale16 / 16;
        sMapLeft = (PANEL_W - width) / 2;
        sMapTop = (MAP_H - height) / 2;
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                // Between the map's own pixels the coverage is blended, so
                // the enlarged track has smooth edges.
                int fx = x * 256 * 16 / sMapScale16 - 128, fy = y * 256 * 16 / sMapScale16 - 128;
                int x0, y0, x1, y1, wx, wy;
                unsigned alpha;
                uint8_t *out = &sMapPicture[((sMapLeft + x) * MAP_H + (MAP_H - 1 - (sMapTop + y))) * 3];

                fx = fx < 0 ? 0 : fx;
                fy = fy < 0 ? 0 : fy;
                x0 = fx >> 8; y0 = fy >> 8;
                wx = fx & 255; wy = fy & 255;
                x1 = x0 + 1 < snap->mapWidth ? x0 + 1 : x0;
                y1 = y0 + 1 < snap->mapHeight ? y0 + 1 : y0;
                if (snap->mapMirrored) {
                    x0 = snap->mapWidth - 1 - x0;
                    x1 = snap->mapWidth - 1 - x1;
                }
                alpha = (snap->map[y0 * snap->mapWidth + x0] * (256 - wx) * (256 - wy) +
                         snap->map[y0 * snap->mapWidth + x1] * wx * (256 - wy) +
                         snap->map[y1 * snap->mapWidth + x0] * (256 - wx) * wy +
                         snap->map[y1 * snap->mapWidth + x1] * wx * wy) >> 16;
                out[0] = (uint8_t) (out[0] + (b - out[0]) * (int) alpha / 255);
                out[1] = (uint8_t) (out[1] + (g - out[1]) * (int) alpha / 255);
                out[2] = (uint8_t) (out[2] + (r - out[2]) * (int) alpha / 255);
            }
        }
    }
}

// A racer on the map; nothing of it goes outside the map's box, where it
// would stay behind.
static void dot(int cx, int cy, int radius, uint32_t colour, uint32_t edge) {
    int x, y;

    for (y = -radius - 1; y <= radius + 1; y++) {
        for (x = -radius - 1; x <= radius + 1; x++) {
            int d = x * x + y * y;

            if (cx + x < PANEL_X || cx + x >= PANEL_X + PANEL_W || cy + y < MAP_Y || cy + y >= MAP_Y + MAP_H) {
                continue;
            }
            if (d <= radius * radius) {
                put(cx + x, cy + y, colour);
            } else if (d <= (radius + 1) * (radius + 1) + 1) {
                put(cx + x, cy + y, edge);
            }
        }
    }
}

static void draw_map(const BottomSnapshot *snap) {
    int x, y, i;

    if (snap->mapSerial != sMapSerial) {
        sMapSerial = snap->mapSerial;
        build_map_picture(snap);
    }
    for (x = 0; x < PANEL_W; x++) {
        memcpy(sCopy + ((PANEL_X + x) * SCREEN_H + (SCREEN_H - (MAP_Y + MAP_H))) * 3, sMapPicture + x * MAP_H * 3,
               MAP_H * 3);
    }
    dirty(PANEL_X, MAP_Y, PANEL_W, MAP_H);
    if (sMapScale16 == 0) {
        text_centred(PANEL_X, PANEL_W, MAP_Y + MAP_H / 2 - 4, "NO MAP HERE", COL_DIM, 1);
        return;
    }
    // This console's racer last, so that it is on top.
    for (i = 0; i <= snap->racerCount; i++) {
        int isMe = i == snap->racerCount;
        const BottomRacer *racer;

        if ((i == snap->me) || (isMe && (snap->me < 0 || snap->me >= snap->racerCount))) {
            continue;
        }
        racer = &snap->racers[isMe ? snap->me : i];
        x = PANEL_X + sMapLeft + racer->mapX * sMapScale16 / 256;
        y = MAP_Y + sMapTop + racer->mapY * sMapScale16 / 256;
        if (x < PANEL_X || x >= PANEL_X + PANEL_W || y < MAP_Y || y >= MAP_Y + MAP_H) {
            continue;       // off the picture: a racer being put back on the track
        }
        dot(x, y, isMe ? 3 : 2, RGB(racer->colour[0], racer->colour[1], racer->colour[2]),
            isMe || racer->player >= 0 ? COL_TEXT : COL_DARK);
    }
}

// The racers in the order of their places: order[0] leads.
static void race_order(const BottomSnapshot *snap, int order[BOTTOM_MAX_RACERS]) {
    int i, j;

    for (i = 0; i < snap->racerCount; i++) {
        int place = snap->racers[i].place >= 1 ? snap->racers[i].place : 99;

        for (j = i; j > 0; j--) {
            int before = snap->racers[order[j - 1]].place >= 1 ? snap->racers[order[j - 1]].place : 99;

            if (before <= place) {
                break;
            }
            order[j] = order[j - 1];
        }
        order[j] = i;
    }
}

static void draw_row(const BottomSnapshot *snap, int index, int rank, int y) {
    const BottomRacer *racer = &snap->racers[index];
    int session = netplay_players() != 0;
    int isMe = index == snap->me && !snap->twoPlayer;
    char line[24], number[4];
    uint32_t colour = RGB(racer->colour[0], racer->colour[1], racer->colour[2]);

    fill(LIST_X, y, LIST_W, ROW_H, isMe ? COL_ROW_ME : COL_ROW);
    if (isMe) {
        frame(LIST_X, y, LIST_W, ROW_H, COL_EDGE_ME);
    }
    fill(LIST_X, y, 2, ROW_H, colour);         // the racer's colour on the map
    snprintf(number, sizeof(number), "%d", rank + 1);
    text_edged(LIST_X + PLACE_X, y + (ROW_H - 16) / 2, number, rank == 0 ? COL_GOLD : COL_TEXT, 2);

    if (racer->portrait != NULL && racer->portraitWidth > 0 &&
        (sPortraitSerial[index] != racer->portraitSerial || sPortraitSource[index] != racer->portrait ||
         sPortraits[index].pixels == NULL)) {
        sPortraitSerial[index] = racer->portraitSerial;
        sPortraitSource[index] = racer->portrait;
        image_scale(racer->portrait, racer->portraitWidth, racer->portraitHeight, &sPortraits[index], PORTRAIT_SIZE,
                    PORTRAIT_SIZE);
    }
    if (racer->portrait != NULL && racer->portraitWidth > 0 && sPortraits[index].pixels != NULL) {
        image_draw(&sPortraits[index], LIST_X + PORTRAIT_X, y + (ROW_H - PORTRAIT_SIZE) / 2);
    } else {
        // No portrait: the racer's colour and initial.
        number[0] = racer->name[0];
        number[1] = '\0';
        fill(LIST_X + PORTRAIT_X, y + 1, PORTRAIT_SIZE, ROW_H - 2, colour);
        text_edged(LIST_X + PORTRAIT_X + 4, y + (ROW_H - 16) / 2, number, COL_TEXT, 2);
    }

    // Two lines beside the portrait: who it is, and what there is to say.
    if (session && racer->player >= 0 && racer->player < netplay_players()) {
        int points = netplay_rating(racer->player);

        snprintf(line, 13, "%s", netplay_player_name(racer->player));
        text_edged(LIST_X + NAME_X, y + 3, line, COL_TEXT, 1);
        int change = netplay_rating_change(racer->player);

        // A player's points; once the race is over and has moved them, what
        // it gave or took in brackets.
        if (racer->finished && change != 0) {
            snprintf(line, 13, "%d (%+d)", points, change);
        } else {
            snprintf(line, 13, "%d PTS", points);
        }
        text(LIST_X + NAME_X, y + 14, line, racer->finished ? COL_GREEN : (points < 0 ? RGB(255, 150, 150) : COL_GOLD), 1);
    } else {
        snprintf(line, 13, "%s", racer->name);
        text_edged(LIST_X + NAME_X, y + 3, line, COL_TEXT, 1);
        if (racer->finished) {
            text(LIST_X + NAME_X, y + 14, "FINISHED", COL_GREEN, 1);
        } else if (racer->player >= 0) {
            if (snap->twoPlayer) {
                snprintf(line, 13, "PLAYER %d", racer->player + 1);
            } else {
                snprintf(line, 13, "YOU");
            }
            text(LIST_X + NAME_X, y + 14, line, COL_GOLD, 1);
        } else if (session) {
            text(LIST_X + NAME_X, y + 14, "CPU", COL_DIM, 1);
        }
    }
}

// The standings. Each row moves a part of the way to its place every poll.
// Returns 1 when it drew.
static int draw_standings(const BottomSnapshot *snap, int force) {
    static unsigned sLastSum;
    int order[BOTTOM_MAX_RACERS] = { 0 };
    int rankOf[BOTTOM_MAX_RACERS] = { 0 };
    unsigned sum = 17;
    int i;

    race_order(snap, order);
    if (sRowCount != snap->racerCount) {
        sRowCount = snap->racerCount;
        force = 2;      // a new race: rows start in their places
    }
    for (i = 0; i < snap->racerCount; i++) {
        int target = (LIST_Y + i * ROW_STEP) * 16;
        int *y = &sRowY[order[i]];

        rankOf[order[i]] = i;
        if (force == 2) {
            *y = target;
        } else if (*y != target) {
            int step = (target - *y) / 5;

            *y += step != 0 ? step : (target > *y ? 1 : -1) * (abs(target - *y) < 16 ? abs(target - *y) : 16);
        }
    }
    for (i = 0; i < snap->racerCount; i++) {
        const BottomRacer *racer = &snap->racers[i];

        sum = sum * 31 + (unsigned) (sRowY[i] / 16);
        sum = sum * 31 + (unsigned) rankOf[i];
        sum = sum * 31 + (unsigned) racer->finished + (unsigned) racer->player * 4 + racer->portraitSerial * 64;
        sum = sum * 31 + (unsigned) (racer->player >= 0 ? netplay_rating(racer->player) : 0);
        sum = sum * 31 + (unsigned) (racer->player >= 0 ? netplay_rating_change(racer->player) : 0);
        sum = sum * 31 + (unsigned) (i == snap->me);
    }
    // Rows in motion are drawn on every second poll: half the work, and still
    // thirty steps a second.
    if (!force && (sum == sLastSum || (sPolls & 1))) {
        return 0;
    }
    sLastSum = sum;
    clear(0, TOP, PANEL_X, SCREEN_H - TOP);
    // Rows that are being overtaken first, the leader and this console's
    // racer over them.
    for (i = snap->racerCount - 1; i >= 0; i--) {
        if (order[i] != snap->me) {
            draw_row(snap, order[i], i, sRowY[order[i]] / 16);
        }
    }
    if (snap->me >= 0 && snap->me < snap->racerCount) {
        draw_row(snap, snap->me, rankOf[snap->me], sRowY[snap->me] / 16);
    }
    return 1;
}

// Speed, lap, bananas, coins; and the line at the foot.
static int draw_info(const BottomSnapshot *snap, int force) {
    static unsigned sLastSum;
    char line[32];
    int idle = netplay_idle_warning();
    int own = snap->me >= 0 && !snap->twoPlayer;
    unsigned sum = (unsigned) snap->speed + (unsigned) snap->bananas * 151 + (unsigned) snap->lap * 151 * 100 +
                   (unsigned) snap->laps * 151 * 1000 + (unsigned) (snap->silverCoins + 1) * 151 * 10000 +
                   (unsigned) idle * 151 * 100000 + (unsigned) own * 7 + (unsigned) snap->adventure * 3 +
                   (unsigned) (snap->collect.balloons + snap->collect.wizpigAmulet * 64 + snap->collect.trophies * 512) *
                       2000003u;

    if (!force && sum == sLastSum) {
        return 0;
    }
    sLastSum = sum;
    clear(PANEL_X, INFO_Y, SCREEN_W - PANEL_X, SCREEN_H - INFO_Y);
    fill(PANEL_X, INFO_Y, PANEL_W, INFO_H, COL_PANEL);
    fill(PANEL_X, COUNT_Y, PANEL_W, COUNT_H, COL_PANEL);
    if (own) {
        text(PANEL_X + 6, INFO_Y + 4, "SPEED", COL_DIM, 1);
        snprintf(line, sizeof(line), "%3d", snap->speed);
        text_edged(PANEL_X + 6, INFO_Y + 14, line, COL_TEXT, 3);
        fill(PANEL_X + 6, INFO_Y + INFO_H - 5, 76, 3, COL_BAR);
        fill(PANEL_X + 6, INFO_Y + INFO_H - 5, 76 * snap->speed / 150, 3, snap->speed >= 100 ? COL_GOLD : COL_GREEN);
        if (snap->laps > 0) {
            text(PANEL_X + 96, INFO_Y + 4, "LAP", COL_DIM, 1);
            snprintf(line, sizeof(line), "%d/%d", snap->lap < 1 ? 1 : snap->lap, snap->laps);
            text_edged(PANEL_X + 96, INFO_Y + 16, line, COL_TEXT, 2);
        }
        banana_count(PANEL_X + 6, COUNT_Y + (COUNT_H - (sBananaSmall.pixels != NULL ? sBananaSmall.h : 8)) / 2,
                     &sBananaSmall, snap->bananas, 2);
        if (snap->silverCoins >= 0) {
            snprintf(line, sizeof(line), "COINS %d/8", snap->silverCoins);
            text(PANEL_X + PANEL_W - 6 - text_width(line, 1), COUNT_Y + 8, line,
                 snap->silverCoins >= 8 ? COL_GREEN : RGB(200, 210, 225), 1);
        }
    }
    if (idle != 0) {
        // Thirty seconds without input in a race; thirty more and the
        // player is out (pc_session_watch_idle in src/objects.c).
        fill(PANEL_X, FOOT_Y, PANEL_W, SCREEN_H - FOOT_Y, COL_RED);
        if (idle == netplay_slot() + 1) {
            text_centred(PANEL_X, PANEL_W, FOOT_Y + 2, "YOU ARE INACTIVE", COL_TEXT, 1);
            text_centred(PANEL_X, PANEL_W, FOOT_Y + 12, "move or be dropped", COL_TEXT, 1);
        } else {
            snprintf(line, sizeof(line), "PLAYER %d INACTIVE", idle);
            text_centred(PANEL_X, PANEL_W, FOOT_Y + 2, line, COL_TEXT, 1);
            text_centred(PANEL_X, PANEL_W, FOOT_Y + 12, "will be dropped", COL_TEXT, 1);
        }
    } else if (snap->adventure) {
        // The Adventure's collectables, in short; the hub has them all.
        snprintf(line, sizeof(line), "BALLOONS %d/47", snap->collect.balloons);
        shade(PANEL_X, FOOT_Y, PANEL_W, SCREEN_H - FOOT_Y, 100);
        text(PANEL_X + 2, FOOT_Y + 2, line, COL_GOLD, 1);
        snprintf(line, sizeof(line), "AMULET %d/4 CUPS %d/5", snap->collect.wizpigAmulet, snap->collect.trophies);
        text(PANEL_X + 2, FOOT_Y + 12, line, COL_TEXT, 1);
    } else {
        shade(PANEL_X, FOOT_Y + 4, PANEL_W, 16, 120);
        text_centred(PANEL_X, PANEL_W, FOOT_Y + 8, "SELECT: settings", COL_TEXT, 1);
    }
    return 1;
}

// ---- which page, and when

#define STALE_POLLS 20      // polls without a frame of a level before the race page gives way

enum { PAGE_NONE, PAGE_MENU, PAGE_HUB, PAGE_RACE };

static int sPage = PAGE_NONE;

static size_t sFlushAt, sFlushSize;         // the frame buffer bytes the last flush wrote

void bottomscreen_last_change(unsigned *offset, unsigned *size) {
    *offset = (unsigned) sFlushAt;
    *size = (unsigned) sFlushSize;
}

static void flush(uint8_t *frameBuffer) {
    int x;

    if (sDirtyX0 < 0) sDirtyX0 = 0;
    if (sDirtyY0 < TOP) sDirtyY0 = TOP;
    if (sDirtyX1 > SCREEN_W) sDirtyX1 = SCREEN_W;
    if (sDirtyY1 > SCREEN_H) sDirtyY1 = SCREEN_H;
    for (x = sDirtyX0; x < sDirtyX1 && sDirtyY1 > sDirtyY0; x++) {
        size_t at = ((size_t) x * SCREEN_H + (size_t) (SCREEN_H - sDirtyY1)) * 3;

        memcpy(frameBuffer + at, sCopy + at, (size_t) (sDirtyY1 - sDirtyY0) * 3);
    }
    sFlushAt = (size_t) sDirtyX0 * SCREEN_H * 3;
    sFlushSize = sDirtyX1 > sDirtyX0 ? (size_t) (sDirtyX1 - sDirtyX0) * SCREEN_H * 3 : 0;
    sDirtyX0 = sDirtyX1 = 0;
}

int bottomscreen_tick(unsigned char *frameBuffer, int redraw) {
    static unsigned sLastFrame;
    static int sStale = STALE_POLLS, sLastBank = -1, sLastCollect = -1;
    static char sLastNote[sizeof(sNote)];
    const BottomSnapshot *snap = pc_bottom_snapshot();
    int scene, page, drew = 0;

    sPolls++;
    if (snap->frame != sLastFrame) {
        sLastFrame = snap->frame;
        sStale = 0;
    } else if (sStale < STALE_POLLS) {
        sStale++;
    }
    scene = sStale < STALE_POLLS ? snap->scene : BOTTOM_SCENE_NONE;
    if (scene == BOTTOM_SCENE_RACE && snap->racerCount > 0) {
        page = PAGE_RACE;
    } else if (netplay_players() != 0 || netplay_lobby_code()[0] != '\0') {
        sPage = PAGE_NONE;
        return -1;      // a session's lobby and menus: touchmenu.c says which player this is
    } else if (scene == BOTTOM_SCENE_HUB && snap->adventure) {
        page = PAGE_HUB;
    } else {
        page = PAGE_MENU;
    }
    if (page != sPage) {
        sPage = page;
        redraw = 1;
    }

    if (page == PAGE_RACE) {
        if (redraw) {
            clear(0, TOP, SCREEN_W, SCREEN_H - TOP);
        }
        drew |= draw_standings(snap, redraw);
        // The dots move every frame; two polls in three is smooth enough
        // and leaves the third to the game.
        if (redraw || sPolls % 3 != 0) {
            draw_map(snap);
            drew = 1;
        }
        if (redraw || sPolls % 4 == 0) {
            drew |= draw_info(snap, redraw);
        }
    } else if (page == PAGE_HUB) {
        int collect = snap->collect.balloons + snap->collect.keys * 64 + snap->collect.wizpigAmulet * 512 +
                      snap->collect.ttAmulet * 4096 + snap->collect.trophies * 32768 + snap->collect.bosses * 262144;

        if (redraw || collect != sLastCollect || modchar_bananas() != sLastBank) {
            sLastCollect = collect;
            sLastBank = modchar_bananas();
            draw_hub_page(snap);
            drew = 1;
        }
    } else if (redraw || modchar_bananas() != sLastBank || strcmp(sNote, sLastNote) != 0) {
        sLastBank = modchar_bananas();
        memcpy(sLastNote, sNote, sizeof(sNote));
        draw_menu_page();
        drew = 1;
    }
    if (drew) {
        flush(frameBuffer);
    }
    return drew;
}

void bottomscreen_init(const char *dir) {
    char path[160];
    Image sky;
    int x, y;

    // The background: a still picture, so that it costs a copy and no more.
    snprintf(path, sizeof(path), "%s/bottom/sky.bin", dir);
    image_load(path, &sky);
    for (x = 0; x < SCREEN_W; x++) {
        for (y = 0; y < SCREEN_H; y++) {
            uint32_t colour = sky.pixels != NULL && x < sky.w && y < sky.h ? sky.pixels[y * sky.w + x] : COL_BACK;
            uint8_t *p = sBack + (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;

            p[0] = (uint8_t) colour;
            p[1] = (uint8_t) (colour >> 8);
            p[2] = (uint8_t) (colour >> 16);
        }
    }
    printf("BOTTOM: sky %s\n", sky.pixels != NULL ? "loaded" : "not found (plain background)");
    free(sky.pixels);

    snprintf(path, sizeof(path), "%s/bottom/logo.bin", dir);
    image_load(path, &sLogo);
    snprintf(path, sizeof(path), "%s/bottom/banana.bin", dir);
    image_load(path, &sBanana);
    if (sBanana.pixels != NULL) {
        image_scale(sBanana.pixels, sBanana.w, sBanana.h, &sBananaBig, sBanana.w * 40 / sBanana.h, 40);
        image_scale(sBanana.pixels, sBanana.w, sBanana.h, &sBananaSmall, sBanana.w * 20 / sBanana.h, 20);
    }
    printf("BOTTOM: logo %s, banana %s\n", sLogo.pixels != NULL ? "loaded" : "not found (text instead)",
           sBanana.pixels != NULL ? "loaded" : "not found (text instead)");
}
