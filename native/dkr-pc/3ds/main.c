#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>

#include "gfx.h"

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread, same as
// the OoT DC port calling Main()/Graph_ThreadEntry directly — the N64 boot
// chain (mainproc -> thread1 -> thread3) is skipped entirely, so the libultra
// thread machinery is never needed.
void thread3_main(void *unused);

// libultra code asks about "the current thread" (osGetThreadPri(NULL) etc.).
// There is no thread system on PC, so the host thread poses as one: thread 3
// at its real priority.
extern OSThread *__osRunningThread;
static OSThread sHostThread;

// Where a frame's time goes, summed for the once-a-second report (autotest.c).
extern u64 svcGetSystemTick(void);
u64 gProfDlTicks, gProfPresentTicks, gProfAudioTicks, gProfGpuWaitTicks;
f32 gProfGpuMs, gProfGpuMaxMs;      // the GPU's own time per frame, summed and the longest
volatile u64 gRenderCostTicks;
u32 gProfTriVerts;
u32 gProfTexCreates, gProfTexTexels, gProfSlowShades, gProfBatches;
static u32 sProfTriVerts;

// Where the interpreter's own time goes, for a build made with PROFILE=1
// (Makefile.3ds): a line in the log every 60 frames. The parts overlap:
// "polygons" contains "combiner" and "texture".
#ifdef DL_PROFILE
enum { PF_VERTEX, PF_POLYGON, PF_COMBINER, PF_TEXTURE, PF_MATRIX, PF_RECT, PF_COUNT };
static u64 sPfTicks[PF_COUNT];
static u32 sPfCalls[PF_COUNT];
static u32 sPfInputVerts;
#define PF_BEGIN u64 pfStart = svcGetSystemTick()
#define PF_END(part) (sPfTicks[part] += svcGetSystemTick() - pfStart, sPfCalls[part]++)
static void pf_report(u64 total) {
    static const char *const names[PF_COUNT] = { "vertex loads", "polygons", "combiner", "texture", "matrix", "rectangles" };
    static u64 sTotal;
    static u32 sFrames;
    s32 i;

    sTotal += total;
    if (++sFrames < 60) {
        return;
    }
    printf("dlprof: per frame %.2f ms; %lu vertices loaded;", (double) sTotal / 60.0 / 268111.856, (unsigned long) (sPfInputVerts / 60));
    for (i = 0; i < PF_COUNT; i++) {
        printf(" %s %.2f ms in %lu;", names[i], (double) sPfTicks[i] / 60.0 / 268111.856, (unsigned long) (sPfCalls[i] / 60));
        sPfTicks[i] = 0;
        sPfCalls[i] = 0;
    }
    printf("\n");
    sTotal = 0;
    sFrames = 0;
    sPfInputVerts = 0;
}
#else
#define PF_BEGIN
#define PF_END(part)
#endif

void pc_host_thread_init(void) {
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
}

// ---------------------------------------------------------------------------
// Frame pacing — stands in for the VI retrace interrupt. The return value is
// the game's logic update rate: the number of retrace periods this frame covers,
// which obj_update() uses directly as its physics timestep.
//
// It must be STABLE, not merely accurate. Retail fb_update() (src/video.c)
// commits to a rate (gVideoDeltaTime, initialised to LOGIC_30FPS = 2) and
// *blocks* to pad a fast frame out to it, only lowering the rate after 20
// consecutive frames disagree. Returning the raw measured period count instead
// lets the timestep oscillate 1,2,1,1,2 with wall-clock noise, and integrating
// physics with a jittering timestep makes everything visibly shake in place.
//
// So: pace to the same 30Hz cadence vanilla runs at, and only report more
// periods if we genuinely ran slow.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// F3DDKR HLE renderer — untextured, vertex-shaded.
//
// gfxtask_run_xbus (src/rcp_dkr.c) hands us the display list the RSP would have
// run. This interprets the geometry half of the microcode the way the RSP does
// — matrix slots, vertex loads, billboarding, G_TRIN polygons — transforms to
// clip space, perspective divides, maps through the viewport, and hands the
// triangles to the host GL layer (linux/gfx.c), shaded from their vertex
// colours. Everything RDP-side (textures, combiners, blenders, rectangles) is
// ignored for now.
// ---------------------------------------------------------------------------

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define WINDOW_SCALE 3

#define GFX_MAX_VERTS 64    // the RSP's internal vertex array
#define GFX_MAX_DEPTH 16    // display-list recursion guard
#define GFX_MAX_TRI_VERTS 8192

// A vertex in clip space — post-matrix, pre-divide. Kept unprojected because
// near-plane clipping has to interpolate here, before the divide by w (a vertex
// behind the camera has w <= 0, and dividing by it is exactly the garbage the
// clip exists to prevent).
typedef struct {
    f32 clip[4];
    f32 u, v; // texel coords, filled in per-triangle from the Triangle's UVs
    u8 r, g, b, a;
    u8 wide;  // drawn through a perspective matrix: part of the world
} GfxVertex;

// ---------------------------------------------------------------------------
// Widescreen. The 3DS top screen is 400 pixels wide against the game's 320,
// and the host layer shows N64 x from -40 to 360. The world (anything drawn
// through a perspective matrix, which the game builds for 5:3 on this target)
// has its viewports spread 1.25x about the screen centre, so it fills the
// screen with a wider view rather than a stretched one. The 2D layer (HUD,
// text, menus) keeps its size and sits in the middle 320 columns, except
// rectangles that span the whole 320, which are backgrounds and fades and
// are widened to cover the screen.
//
// A view that is a window in a menu (the track's picture in its wooden frame
// on the track select and after a race) is the exception: its frame is 2D and
// keeps its place, so the window does too. Its world is still drawn 1.25x as
// wide, for the 5:3 projection, about the window's own centre, and what falls
// outside the window is cut off. Only a side of a view that lies on the edge
// of the 320 columns moves out to the edge of the screen.
// ---------------------------------------------------------------------------
#define WIDE_SCALE 1.25f
#define WIDE_LEFT (-40.0f)
#define WIDE_RIGHT 360.0f
#define WIDE_MARGIN 40.0f
enum { CLIP_NARROW, CLIP_WIDE, CLIP_FULL, CLIP_RECT };

// ---------------------------------------------------------------------------
// RDP texture state. DKR uses the stock texture commands (gDPLoadTextureBlock
// and friends), which arrive as G_SETTIMG (where the image is) + G_SETTILE (how
// to read it) + G_SETTILESIZE (how big it is) + G_LOADTLUT (palette, for CI).
// TMEM is not emulated: at draw time we decode straight from the image address
// the display list last pointed at, which is what every N64 HLE renderer does
// and works because the game's textures are laid out linearly in RAM.
// ---------------------------------------------------------------------------

// Clip anything closer than this. The N64 clips against w, and w is the
// camera-space depth, so this is the near plane in world units.
#define GFX_NEAR_W 1.0f

// Which screen-space winding is a front face. Screen y runs downwards here, so
// this is the opposite sign from the y-up convention. If the world renders
// inside-out — outer surfaces gone, inner ones visible — flip this.
#define GFX_FRONT_FACE_SIGN (-1.0f)

static u32 sGfxFrameCount = 0;

static f32 sMatrices[3][4][4]; // G_MTX_DKR_INDEX_0..2
static u8 sMatrixWide[3];      // the slot holds a perspective matrix
static u8 sStretchFlat;        // inside PC_STRETCH_MARK: flat geometry covers the wide screen too
static s32 sCurMatrix = 0;
static s32 sBillboard = FALSE;

static GfxVertex sVerts[GFX_MAX_VERTS];
static s32 sVertexBase = 0; // where G_VTX_APPEND vertices land

static f32 sVpScaleX = N64_SCREEN_W / 2.0f;
static f32 sVpScaleY = N64_SCREEN_H / 2.0f;
static f32 sVpTransX = N64_SCREEN_W / 2.0f;
static f32 sVpTransY = N64_SCREEN_H / 2.0f;
// The same viewport for the world on the wide screen (apply_viewport).
static f32 sVpWideScaleX = (N64_SCREEN_W / 2.0f) * WIDE_SCALE;
static f32 sVpWideTransX = N64_SCREEN_W / 2.0f;

// ---------------------------------------------------------------------------
// What gets clipped away, in screen pixels, x1/y1 exclusive.
//
// Two rectangles, and the drawing is confined to the intersection:
//
//   sScis*  — the RDP scissor (G_SETSCISSOR). Split-screen, text boxes.
//   sVpClip* — the viewport's own bounds. The RSP clips geometry to the view
//       volume, and the viewport maps NDC +-1 onto exactly this rectangle, so on
//       hardware nothing can be drawn outside it. We only clip against the near
//       plane, so without this a small viewport — the track-preview window in the
//       level-select menu — lets its geometry spill out across the whole screen.
//
// Rectangles (text, HUD pictures, dialogue boxes, fades) never pass through the
// RSP: the scissor alone cuts them (CLIP_RECT, CLIP_FULL). Cut by the viewport
// too, whatever was written while the last view's viewport lay off the scissor
// had nothing left to show in: the character select's heading, "OK?", the
// copyright line.
// ---------------------------------------------------------------------------
static f32 sScisX0, sScisY0, sScisX1, sScisY1;
static f32 sVpClipX0, sVpClipY0, sVpClipX1, sVpClipY1;

static f32 max_f(f32 a, f32 b) {
    return (a > b) ? a : b;
}

static f32 min_f(f32 a, f32 b) {
    return (a < b) ? a : b;
}

static s32 sClipMode = CLIP_NARROW;

// ---------------------------------------------------------------------------
// One player's view per console. In a session between consoles every console
// runs the same game, which lays its frame out as a split screen; the list
// says whose part is whose (PC_VIEW_MARK, include/f3ddkr.h). With a local
// view set, the other players' parts are not drawn and this player's world is
// given the whole screen. The game lays a session's HUD out for the whole
// screen itself (hud_init in src/game_ui.c).
// ---------------------------------------------------------------------------
static s32 sLocalView = -1;     // the player this console shows; -1: draw the frame as laid out
static s32 sMarkPlayer = -1;    // whose commands these are; -1: everybody's
static s32 sMarkCount = 1;      // views in the layout
static f32 sVpOrigScaleX, sVpOrigScaleY, sVpOrigTransX, sVpOrigTransY;

void pc_gfx_set_local_view(s32 player) {
    sLocalView = player;
}

static s32 view_hidden(void) {
    return sLocalView >= 0 && sMarkPlayer >= 0 && sMarkPlayer != sLocalView;
}

static s32 view_remapped(void) {
    return sLocalView >= 0 && sMarkPlayer == sLocalView && sMarkCount > 1;
}

static void apply_clip_rect(void);

// Whether the viewport ends on the left or right edge of the 320 columns
// (within two pixels: a menu's own full-width viewport runs from 0 to 321).
static s32 vp_on_left_edge(void) {
    return sVpClipX0 > -2.0f && sVpClipX0 < 2.0f;
}

static s32 vp_on_right_edge(void) {
    return sVpClipX1 > (f32) N64_SCREEN_W - 2.0f && sVpClipX1 < (f32) N64_SCREEN_W + 2.0f;
}

// The viewport in effect, from the one the game set (sVpOrig*): the game's
// own, or the whole screen for this console's player in a session. Called
// when either the viewport or the view mark changes, since the game sets
// them in either order.
static void apply_viewport(void) {
    f32 halfW, halfH;

    sVpScaleX = sVpOrigScaleX;
    sVpScaleY = sVpOrigScaleY;
    sVpTransX = sVpOrigTransX;
    sVpTransY = sVpOrigTransY;
    if (view_remapped()) {
        // This player's world over the whole screen.
        sVpScaleX = (sVpScaleX < 0.0f) ? -(N64_SCREEN_W / 2.0f) : (N64_SCREEN_W / 2.0f);
        sVpScaleY = (sVpScaleY < 0.0f) ? -(N64_SCREEN_H / 2.0f) : (N64_SCREEN_H / 2.0f);
        sVpTransX = N64_SCREEN_W / 2.0f;
        sVpTransY = N64_SCREEN_H / 2.0f;
    }

    // The rectangle NDC +-1 maps onto. The scale carries a sign (y is
    // commonly negative, since screen y runs the other way from clip y), so
    // take it off before using it as a half-size.
    halfW = (sVpScaleX < 0.0f) ? -sVpScaleX : sVpScaleX;
    halfH = (sVpScaleY < 0.0f) ? -sVpScaleY : sVpScaleY;
    sVpClipX0 = sVpTransX - halfW;
    sVpClipX1 = sVpTransX + halfW;
    sVpClipY0 = sVpTransY - halfH;
    sVpClipY1 = sVpTransY + halfH;

    // The world's viewport on the wide screen: 1.25x as wide, and moved with
    // whichever of its sides lie on the screen's edge. The whole screen stays
    // centred, the left views of a four-way split move 20 to the left and
    // reach -40, and a window in a menu stays where the game put it.
    sVpWideScaleX = sVpScaleX * WIDE_SCALE;
    sVpWideTransX = sVpTransX;
    if (vp_on_left_edge()) {
        sVpWideTransX -= WIDE_MARGIN / 2.0f;
    }
    if (vp_on_right_edge()) {
        sVpWideTransX += WIDE_MARGIN / 2.0f;
    }
    apply_clip_rect();
}

static void apply_clip_rect(void) {
    f32 x0 = max_f(sScisX0, sVpClipX0), x1 = min_f(sScisX1, sVpClipX1);

    if (sClipMode == CLIP_FULL || sClipMode == CLIP_RECT) {
        // A rectangle: the scissor only, and the wide screen's whole width
        // for one that spans the 320 columns.
        if (sClipMode == CLIP_FULL) {
            x0 = WIDE_LEFT;
            x1 = WIDE_RIGHT;
        } else {
            x0 = sScisX0;
            x1 = sScisX1;
        }
        gfx_set_scissor(x0, sScisY0, x1, sScisY1);
        return;
    } else if (sClipMode == CLIP_WIDE) {
        // A view that ends on the edge of the 320 columns (a race) is carried
        // out to the edge of the screen. One that ends elsewhere is a window
        // and is cut where the game cuts it, like the frame drawn around it:
        // at its own sides, and at the 320 columns when it slides off them.
        if (sScisX0 <= 0.0f && vp_on_left_edge()) {
            x0 = WIDE_LEFT;
        }
        if (sScisX1 >= (f32) N64_SCREEN_W && vp_on_right_edge()) {
            x1 = WIDE_RIGHT;
        }
    }
    gfx_set_scissor(x0, max_f(sScisY0, sVpClipY0), x1, min_f(sScisY1, sVpClipY1));
}

static void set_clip_mode(s32 mode) {
    if (mode != sClipMode) {
        sClipMode = mode;
        apply_clip_rect();
    }
}

static GfxTriVert sTriVerts[GFX_MAX_TRI_VERTS];
static s32 sTriVertCount = 0;

static u32 sTimgAddr = 0;  // G_SETTIMG — wherever the DL last pointed
static u32 sTexAddr = 0;   // the image a G_LOADBLOCK/G_LOADTILE actually loaded
static s32 sTexSwapped = 0; // that load had dxt == 0 — see decode_texture()
static u8 sTileFmt = 0;    // G_SETTILE — G_IM_FMT_*
static u8 sTileSiz = 0;    //             G_IM_SIZ_*
static u8 sTilePalette = 0;
static u8 sTileCmS = 0; // the raw 2-bit clamp/mirror fields, not booleans
static u8 sTileCmT = 0;
static u16 sTileWidth = 0; // G_SETTILESIZE
static u16 sTileHeight = 0;
static u16 sTileUls = 0;   //               the tile's origin within the image
static u16 sTileUlt = 0;
static u32 sTlutAddr = 0;  // G_LOADTLUT — palette for the CI formats

// RDP colour state. Only the 2D path reads these: the triangles get their colour
// from the vertices, but a rectangle has no vertex colours, so its colour comes
// entirely from the combiner and these registers.
// G_SETGEOMETRYMODE / G_CLEARGEOMETRYMODE. This is the RSP half of the depth
// state, and DKR drives it directly: material_set() toggles z-compare per material
// with gSPSetGeometryMode(G_ZBUFFER), not through othermode. The RSP only emits
// depth coordinates when G_ZBUFFER is set, so it gates the RDP's Z_CMP/Z_UPD — no
// geometry mode, no depth, whatever the render mode says.
//
// Seeded with what rendermode_reset() (src/textures_sprites.c) sets, so a frame
// that draws before its first geometry-mode command still gets a z-buffer.
#define GFX_GEOMETRY_MODE_INIT (G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER)
static u32 sGeometryMode = GFX_GEOMETRY_MODE_INIT;

static u32 sOtherModeH = 0;      // G_SETOTHERMODE_H / G_RDPSETOTHERMODE
static u32 sOtherModeL = 0;      // G_SETOTHERMODE_L / G_RDPSETOTHERMODE
static u32 sCombineW0 = 0;       // G_SETCOMBINE — the two mux words
static u32 sCombineW1 = 0;
static u8 sPrimColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sEnvColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sFillColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static u8 sBlendColor[4] = { 0x00, 0x00, 0x00, 0xFF }; // G_SETBLENDCOLOR

// Fog. The game recomputes this every frame, per player (src/tracks.c), out of the
// FogData system its fog-changer objects drive. gSPFogPosition packs a multiplier
// and an offset into one G_MOVEWORD, and the RSP turns them into a per-vertex fade
// factor; gDPSetFogColor is the colour that factor fades towards.
static u8 sFogColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static s16 sFogMul = 0;
static s16 sFogOfs = 0;

#define GFX_MAX_TEXTURES 1024
#define GFX_MAX_TEX_TEXELS (256 * 256)

typedef struct {
    u32 timg;
    u32 tlut;
    u8 fmt, siz, cmS, cmT;
    u8 swapped;
    u16 width, height;
    u32 handle;
    u32 lastUsed; // frame number, for eviction when the cache is full
} GfxTexture;

static GfxTexture sTexCache[GFX_MAX_TEXTURES];
static s32 sTexCacheCount = 0;
// Cache slot by image address; a stale entry only costs the scan, since
// every hit is checked against the slot it names.
#define TEX_HASH_SIZE 1024
static u16 sTexHash[TEX_HASH_SIZE];
static u32 sTexDecodeBuf[GFX_MAX_TEX_TEXELS];
static u8 sTexSwizzleBuf[GFX_MAX_TEX_TEXELS * 4]; // worst case: 32bpp

/**
 * Drop every cached texture decoded out of the memory being freed.
 *
 * The cache is keyed on the RAM address a texture was decoded from, and the game
 * recycles those addresses: the textures of a level it has unloaded are handed
 * straight back out to the next one. Without this, a new texture landing on an
 * old address with the same format and size hits the stale entry and draws the
 * previous level's pixels — and the cache, never evicting, fills up and starts
 * refusing to decode anything at all.
 *
 * This takes the freed slot's whole *range*, not just its base address, and that
 * matters: an earlier version compared against the base and therefore matched
 * nothing, ever. A texture's pixels begin at `tex + 1` — past its TextureHeader —
 * and its palette sits at another offset again (`tex_palette_id`), so no cache
 * entry is ever keyed on an allocation's base. The invalidation silently did
 * nothing, which is exactly the two symptoms you would predict: stale textures
 * after a level change, then everything untextured once the cache hit its cap.
 *
 * Called from mempool_free_addr() (src/memory.c), the choke point every free in
 * the game passes through, so a texture's GL object dies exactly when the memory
 * behind it does. The palette is checked too: a CI texture decoded against a
 * freed TLUT is just as stale.
 */
// The game frees memory on its own thread while the render thread may be
// part-way through a frame, so the ranges are noted here and the cache is
// purged of them when the render thread starts its next frame.
#define MAX_PENDING_INVALIDATIONS 256
static struct { u32 lo, hi; } sPendingInvalid[MAX_PENDING_INVALIDATIONS];
static s32 sPendingInvalidCount;
static s32 sPendingInvalidAll;
extern void pc_render_lock(void);
extern void pc_render_unlock(void);

void pc_gfx_invalidate_range(const void *addr, s32 size) {
    pc_render_lock();
    if (sPendingInvalidCount < MAX_PENDING_INVALIDATIONS) {
        sPendingInvalid[sPendingInvalidCount].lo = (u32) addr;
        sPendingInvalid[sPendingInvalidCount].hi = (u32) addr + (u32) size;
        sPendingInvalidCount++;
    } else {
        sPendingInvalidAll = TRUE;
    }
    pc_render_unlock();
}

static void invalidate_range_now(u32 lo, u32 hi) {
    s32 i;

    for (i = 0; i < sTexCacheCount;) {
        GfxTexture *t = &sTexCache[i];

        if ((t->timg >= lo && t->timg < hi) || (t->tlut >= lo && t->tlut < hi)) {
            gfx_delete_texture(t->handle);
            sTexCache[i] = sTexCache[--sTexCacheCount];
        } else {
            i++;
        }
    }
}

/**
 * Unpack an N64 Mtx (s15.16 fixed point: the integer halves live in the first 8
 * words, the fractional halves in the last 8) into floats — the inverse of
 * mtxf_to_mtx() in src/hasm/math_util.c. Deliberately kept in the same format
 * the RSP consumes, so the PC build sees the same rounding and the same +-32768
 * saturation the console does and the N64 ROM stays a usable oracle.
 */
static void mtx_to_float(const Mtx *m, f32 out[4][4]) {
#ifdef GBI_FLOAT_MTX
    // Float matrix ABI: mtxf_to_mtx()/guMtxF2L() stored the 16 floats raw, so
    // read them back as-is. No fixed unpack, no /65536, no bit-shuffle.
    __builtin_memcpy(out, m, 16 * sizeof(f32));
#else
    const s32 *ints = (const s32 *) &m->m[0][0];
    const s32 *fracs = ints + 8;
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 idx = (i * 2) + (j >> 1);
            s32 fixed;

            if ((j & 1) == 0) {
                fixed = (ints[idx] & 0xFFFF0000) | ((fracs[idx] >> 16) & 0xFFFF);
            } else {
                fixed = (ints[idx] << 16) | (fracs[idx] & 0xFFFF);
            }
            out[i][j] = (f32) fixed / 65536.0f;
        }
    }
#endif
}

/**
 * Transform a vertex the way the RSP does: as a row vector times the selected
 * matrix (the same convention as mtxf_transform_point), then — when
 * billboarding — add the anchor's clip coordinates, then perspective divide and
 * map through the viewport.
 */
static void load_vertex(GfxVertex *dst, const Vertex *v, const f32 *anchor) {
    const f32 (*m)[4] = sMatrices[sCurMatrix];
    f32 x = v->x, y = v->y, z = v->z;
    s32 i;

    for (i = 0; i < 4; i++) {
        dst->clip[i] = (m[0][i] * x) + (m[1][i] * y) + (m[2][i] * z) + m[3][i];
        if (anchor != NULL) {
            dst->clip[i] += anchor[i];
        }
    }

    dst->r = v->r;
    dst->g = v->g;
    dst->b = v->b;
    // Shade alpha. The combiner uses it as a blend weight (SHADE_ALPHA), and the
    // RENDER_VTX_ALPHA materials use it as real translucency — which is why it is
    // mutually exclusive with fog in material_set(): the RSP keeps vertex alpha in
    // the fog slot.
    dst->a = v->a;
    // A billboard's corners are offsets from an anchor in the world.
    dst->wide = (anchor != NULL) ? sVerts[0].wide : (sMatrixWide[sCurMatrix] | sStretchFlat);
}

static void handle_vertex(u32 w0, u32 w1) {
    const Vertex *src = (const Vertex *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = ((params >> 3) & 0x1F) + 1;
    s32 append = params & G_VTX_APPEND;
    s32 dstIdx;
    s32 i;

    if (src == NULL) {
        return;
    }
    PF_BEGIN;

    if (append) {
        dstIdx = sVertexBase;
    } else {
        // Non-appended vertices always go to the start of the array, and their
        // count becomes the base that appended vertices land after.
        dstIdx = 0;
        sVertexBase = count;
    }

    for (i = 0; i < count && dstIdx + i < GFX_MAX_VERTS; i++) {
        // Billboarded vertices are sprite-space offsets from vertex 0, the
        // anchor pushed just before the billboard matrix.
        load_vertex(&sVerts[dstIdx + i], &src[i], sBillboard ? sVerts[0].clip : NULL);
    }
#ifdef DL_PROFILE
    sPfInputVerts += (u32) count;
#endif
    PF_END(PF_VERTEX);
}

/** Expand an N64 RGBA5551 texel (big-endian in the asset) to RGBA8888. */
static u32 rgba16_to_rgba32(u16 texel) {
    u32 r = (texel >> 11) & 0x1F;
    u32 g = (texel >> 6) & 0x1F;
    u32 b = (texel >> 1) & 0x1F;
    u32 a = (texel & 1) ? 0xFF : 0x00;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return r | (g << 8) | (b << 16) | (a << 24);
}

static u32 ia_to_rgba32(u32 intensity, u32 alpha) {
    return intensity | (intensity << 8) | (intensity << 16) | (alpha << 24);
}

/**
 * Undo the N64's odd-row word swizzle.
 *
 * TMEM stores odd rows of a texture with the two 32-bit halves of each 64-bit
 * word exchanged. A normal gDPLoadTextureBlock passes a nonzero dxt, which makes
 * the RDP apply that swizzle as it streams the block in, and the texture fetch
 * unit undoes it on the way out — so the bytes in RAM are plain linear and we can
 * read them as-is.
 *
 * DKR's RENDER_LINE_SWAP textures ("Texture has swapped lines, for speed") do not
 * work that way. They go through gDPLoadTextureBlockS, whose only difference is
 * that it passes dxt = 0 (include/PR/gbi.h:2639) — so the RDP does no swizzle on
 * load, and the asset is instead stored *pre-swizzled* in ROM, letting the fetch
 * unit's unswizzle produce the correct image for free. Reading those bytes
 * linearly, as we did, leaves every odd row with its 4-byte groups exchanged in
 * pairs: the sprite stays entirely recognisable but its odd scanlines are
 * displaced in short runs, which is the serrated comb along the edges of the HUD
 * text.
 *
 * So: when the load had dxt == 0, walk the rows back through the same swap before
 * decoding. `b ^ 4` is the swizzle; it is its own inverse. Rows narrower than 8
 * bytes have nothing to exchange, and the bounds check covers a trailing partial
 * group.
 */
static void unswizzle_rows(const u8 *src, u8 *dst, u8 siz, s32 rowBytes, s32 height) {
    // The swizzle exchanges the 32-bit halves of each 64-bit TMEM word, so it is
    // a ^4 on the byte address *within TMEM*. For 4/8/16-bit texels, TMEM holds
    // the image exactly as RAM does and the ^4 carries straight over.
    //
    // 32-bit texels do not: the RDP splits them across two TMEM banks, red/green
    // in the low one and blue/alpha in the high (which is why the GBI computes
    // their line with G_IM_SIZ_32b_LINE_BYTES = 2, not 4). Each bank therefore
    // holds 2 bytes per texel, so a ^4 in bank-local address space exchanges
    // *pairs* of texels — texel ^ 2 — which in the linear 4-byte-per-texel RAM
    // image we decode from is a ^8. Using ^4 here instead scrambles the texels
    // singly rather than in pairs: still recognisable, subtly wrong. That was the
    // static banana.
    s32 unit = (siz == G_IM_SIZ_32b) ? 8 : 4;
    s32 total = rowBytes * height;
    s32 y, b;

    for (y = 0; y < height; y++) {
        for (b = 0; b < rowBytes; b++) {
            // The ^ is against the offset into the whole image, not into the row:
            // a block load fills TMEM contiguously, and the two only coincide
            // when a row is a whole number of 64-bit words.
            s32 at = (y * rowBytes) + b;
            s32 from = (y & 1) ? (at ^ unit) : at;

            dst[at] = src[(from < total) ? from : at];
        }
    }
}

/** How many bytes one row of a `width`-texel image of this size occupies. */
static s32 row_bytes(u8 siz, s32 width) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return (width + 1) / 2;
        case G_IM_SIZ_8b:
            return width;
        case G_IM_SIZ_16b:
            return width * 2;
        default:
            return width * 4;
    }
}

/**
 * Decode one of the N64 texture formats into RGBA8888. Everything is read a
 * byte at a time, so the assets staying big-endian doesn't matter here.
 */
static void decode_texture(const u8 *src, u8 fmt, u8 siz, s32 width, s32 height, const u8 *tlut, u32 *out) {
    s32 count = width * height;
    s32 i;

    switch ((fmt << 2) | siz) {
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = rgba16_to_rgba32((src[i * 2] << 8) | src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_32b:
            for (i = 0; i < count; i++) {
                out[i] = src[i * 4] | (src[(i * 4) + 1] << 8) | (src[(i * 4) + 2] << 16) | (src[(i * 4) + 3] << 24);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 idx = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 idx = src[i];
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i * 2], src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 hi = src[i] >> 4;
                u32 lo = src[i] & 0xF;
                out[i] = ia_to_rgba32((hi << 4) | hi, (lo << 4) | lo);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                u32 intensity = (texel >> 1) & 7;
                intensity = (intensity << 5) | (intensity << 2) | (intensity >> 1);
                out[i] = ia_to_rgba32(intensity, (texel & 1) ? 0xFF : 0x00);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i], src[i]);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                texel = (texel << 4) | texel;
                out[i] = ia_to_rgba32(texel, texel);
            }
            break;
        default:
            for (i = 0; i < count; i++) {
                out[i] = 0xFFFF00FF; // magenta: an unhandled format, so it's obvious
            }
            break;
    }
}

/**
 * The texture the current RDP state describes, decoded and uploaded once and
 * then kept. Returns 0 (untextured) if there is nothing sane to draw with.
 */
static u32 texture_current(void) {
    const u8 *tlut = (const u8 *) sTlutAddr;
    s32 i;

    if (sTexAddr == 0 || sTileWidth == 0 || sTileHeight == 0) {
        return 0;
    }
    if (sTileWidth * sTileHeight > GFX_MAX_TEX_TEXELS) {
        return 0;
    }
    if (sTileFmt == G_IM_FMT_CI) {
        if (sTlutAddr == 0) {
            return 0;
        }
        // CI4 palettes are 16 entries each, selected by the tile's palette field.
        if (sTileSiz == G_IM_SIZ_4b) {
            tlut += sTilePalette * 16 * 2;
        }
    }

    // Most lookups repeat a recent one: a small table of cache slots by
    // image address answers those without the scan.
    {
        u32 slot = sTexHash[((u32) sTexAddr >> 4) & (TEX_HASH_SIZE - 1)];

        if (slot < (u32) sTexCacheCount) {
            GfxTexture *t = &sTexCache[slot];
            if (t->timg == sTexAddr && t->tlut == (u32) tlut && t->fmt == sTileFmt && t->siz == sTileSiz &&
                t->width == sTileWidth && t->height == sTileHeight && t->cmS == sTileCmS && t->cmT == sTileCmT &&
                t->swapped == (u8) sTexSwapped) {
                t->lastUsed = sGfxFrameCount;
                return t->handle;
            }
        }
    }
    for (i = 0; i < sTexCacheCount; i++) {
        GfxTexture *t = &sTexCache[i];
        if (t->timg == sTexAddr && t->tlut == (u32) tlut && t->fmt == sTileFmt && t->siz == sTileSiz &&
            t->width == sTileWidth && t->height == sTileHeight && t->cmS == sTileCmS && t->cmT == sTileCmT &&
            t->swapped == (u8) sTexSwapped) {
            t->lastUsed = sGfxFrameCount;
            sTexHash[((u32) sTexAddr >> 4) & (TEX_HASH_SIZE - 1)] = (u16) i;
            return t->handle;
        }
    }

    // A full cache used to mean "draw untextured, forever". Evict the entry that
    // has gone unused the longest instead — a texture the game still wants will
    // simply be decoded again next time it asks for it. With invalidation working
    // this should not trigger, but degrading into a slow frame beats degrading
    // into a permanently untextured world.
    if (sTexCacheCount >= GFX_MAX_TEXTURES) {
        s32 oldest = 0;

        for (i = 1; i < sTexCacheCount; i++) {
            if (sTexCache[i].lastUsed < sTexCache[oldest].lastUsed) {
                oldest = i;
            }
        }
        gfx_delete_texture(sTexCache[oldest].handle);
        sTexCache[oldest] = sTexCache[--sTexCacheCount];
    }

    {
        const u8 *texels = (const u8 *) sTexAddr;
        s32 rowBytes = row_bytes(sTileSiz, sTileWidth);

        // A dxt of 0 means the asset is stored pre-swizzled; put it back before
        // decoding. Guarded on the scratch buffer, which a sane texture never
        // exceeds — decoding the raw bytes is better than reading past it.
        if (sTexSwapped && (rowBytes * sTileHeight) <= (s32) sizeof(sTexSwizzleBuf)) {
            unswizzle_rows(texels, sTexSwizzleBuf, sTileSiz, rowBytes, sTileHeight);
            texels = sTexSwizzleBuf;
        }
        decode_texture(texels, sTileFmt, sTileSiz, sTileWidth, sTileHeight, tlut, sTexDecodeBuf);
    }

    {
        GfxTexture *t = &sTexCache[sTexCacheCount++];
        t->timg = sTexAddr;
        t->tlut = (u32) tlut;
        t->fmt = sTileFmt;
        t->siz = sTileSiz;
        t->swapped = (u8) sTexSwapped;
        t->width = sTileWidth;
        t->height = sTileHeight;
        t->cmS = sTileCmS;
        t->cmT = sTileCmT;
        t->lastUsed = sGfxFrameCount;
        gProfTexCreates++;
        gProfTexTexels += (u32) (sTileWidth * sTileHeight);
        t->handle = gfx_create_texture(sTexDecodeBuf, sTileWidth, sTileHeight, sTileCmS, sTileCmT);
        return t->handle;
    }
}

/**
 * Perspective divide plus the viewport map — the last thing the RSP does before
 * handing a vertex to the RDP. Only ever called on clipped vertices, so w is
 * guaranteed positive here.
 */
static void combiner_eval(const f32 shade[4], u8 out[4], u8 sec[3]);
#define COMBINER_CACHE 64   // a power of two

// ---------------------------------------------------------------------------
// The combiner's answer for a vertex, without running it per vertex.
//
// With the texel taken as white, what the combiner computes from a vertex's
// shade is (A - B) * C + D with SHADE or SHADE_ALPHA in some of the slots and
// constants in the rest: bilinear in the shade colour and the shade alpha,
// per channel. Four evaluations at the corners (colour 0 or 1, alpha 0 or 1)
// therefore fix it for every vertex of a material, and a fifth in the middle
// checks that the mode really is of that shape; one that is not (SHADE in
// both A and C, say) falls back to the full evaluation.
// ---------------------------------------------------------------------------
typedef struct {
    s32 base[4];        // colour 0, alpha 0
    s32 dColor[3];      // + per unit of shade colour
    s32 dAlpha[4];      // + per unit of shade alpha
    s32 dBoth[3];       // + per unit of colour * alpha
    s32 exact;          // the model holds for the current combiner
} ShadeModel;
static ShadeModel sShadeModel;

// Everything the combiner reads besides the shade.
typedef struct {
    u32 combineW0, combineW1, otherModeH;
    u32 prim, env;
} CombinerKey;

static void combiner_key(CombinerKey *key) {
    key->combineW0 = sCombineW0;
    key->combineW1 = sCombineW1;
    key->otherModeH = sOtherModeH;
    memcpy(&key->prim, sPrimColor, 4);
    memcpy(&key->env, sEnvColor, 4);
}

static u32 combiner_key_slot(const CombinerKey *key) {
    u32 h = key->combineW0 * 0x9E3779B1u ^ key->combineW1 * 0x85EBCA6Bu ^ key->otherModeH ^ key->prim * 0xC2B2AE35u ^
            key->env * 0x27D4EB2Fu;

    return (h ^ (h >> 15)) & (COMBINER_CACHE - 1);
}

// A track is a few hundred batches a frame but a handful of materials, so
// most batches find the model of the one before still stands (sShadeKey).
// Those that do not mostly return to a material met before: the materials
// alternate, an object's with the track's, forty or more times a frame, and
// working a model out is ten runs of the combiner. So the models are kept,
// by everything they depend on.
static CombinerKey sShadeKey;
static s32 sShadeKeyValid;
static struct {
    CombinerKey key;
    ShadeModel model;
    s32 valid;
} sShadeCache[COMBINER_CACHE];

static void shade_model_prepare(void) {
    u8 c00[4], c10[4], c01[4], c11[4], mid[4], unlit[3];
    CombinerKey key;
    f32 shade[4];
    u32 slot;
    s32 i;

    combiner_key(&key);
    if (sShadeKeyValid && memcmp(&key, &sShadeKey, sizeof(key)) == 0) {
        return;
    }
    sShadeKey = key;
    sShadeKeyValid = TRUE;
    slot = combiner_key_slot(&key);
    if (sShadeCache[slot].valid && memcmp(&key, &sShadeCache[slot].key, sizeof(key)) == 0) {
        sShadeModel = sShadeCache[slot].model;
        return;
    }
    PF_BEGIN;

#define SHADE_SAMPLE(out, s, a) \
    shade[0] = shade[1] = shade[2] = (s); shade[3] = (a); combiner_eval(shade, out, unlit)
    SHADE_SAMPLE(c00, 0.0f, 0.0f);
    SHADE_SAMPLE(c10, 1.0f, 0.0f);
    SHADE_SAMPLE(c01, 0.0f, 1.0f);
    SHADE_SAMPLE(c11, 1.0f, 1.0f);
    SHADE_SAMPLE(mid, 0.5f, 0.5f);
#undef SHADE_SAMPLE

    sShadeModel.exact = TRUE;
    for (i = 0; i < 4; i++) {
        s32 predicted;

        sShadeModel.base[i] = c00[i];
        sShadeModel.dAlpha[i] = c01[i] - c00[i];
        if (i < 3) {
            sShadeModel.dColor[i] = c10[i] - c00[i];
            sShadeModel.dBoth[i] = c11[i] - c10[i] - c01[i] + c00[i];
            predicted = c00[i] + (sShadeModel.dColor[i] + sShadeModel.dAlpha[i]) / 2 + sShadeModel.dBoth[i] / 4;
        } else {
            predicted = c00[i] + sShadeModel.dAlpha[i] / 2;
            if (c10[i] != c00[i]) {
                sShadeModel.exact = FALSE;      // alpha that follows the shade colour
            }
        }
        if (predicted - mid[i] > 3 || mid[i] - predicted > 3) {
            sShadeModel.exact = FALSE;
        }
    }
    sShadeCache[slot].key = key;
    sShadeCache[slot].model = sShadeModel;
    sShadeCache[slot].valid = TRUE;
    PF_END(PF_COMBINER);
}

static inline u8 shade_clamp(s32 v) {
    return (u8) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void shade_vertex(const GfxVertex *v, GfxTriVert *out) {
    if (sShadeModel.exact) {
        const s32 a = v->a;
        const s32 c[3] = { v->r, v->g, v->b };
        u8 rgb[3];
        s32 i;

        for (i = 0; i < 3; i++) {
            // x / 255 as (x * 257 + 32768) >> 16, x / 65025 as x >> 16:
            // within one step of exact, and no division on the ARM11.
            rgb[i] = shade_clamp(sShadeModel.base[i] +
                                 (((sShadeModel.dColor[i] * c[i] + sShadeModel.dAlpha[i] * a) * 257 + 32768) >> 16) +
                                 ((sShadeModel.dBoth[i] * c[i] * a) >> 16));
        }
        out->r = rgb[0];
        out->g = rgb[1];
        out->b = rgb[2];
        out->a = shade_clamp(sShadeModel.base[3] + ((sShadeModel.dAlpha[3] * a * 257 + 32768) >> 16));
    } else {
        f32 shade[4];
        u8 lit[4];
        u8 unlit[3];

        shade[0] = v->r / 255.0f;
        shade[1] = v->g / 255.0f;
        shade[2] = v->b / 255.0f;
        shade[3] = v->a / 255.0f;
        combiner_eval(shade, lit, unlit);
        out->r = lit[0];
        out->g = lit[1];
        out->b = lit[2];
        out->a = lit[3];
    }
}

static void project(const GfxVertex *v, GfxTriVert *out) {
    f32 invW = 1.0f / v->clip[3];

    if (v->wide) {
        out->x = (v->clip[0] * invW * sVpWideScaleX) + sVpWideTransX;
    } else {
        out->x = (v->clip[0] * invW * sVpScaleX) + sVpTransX;
        // Flat geometry that reaches the edge of the 320 columns (screen
        // wipes, full-screen masks) reaches the edge of the wide screen.
        if (out->x <= 0.0f) {
            out->x += WIDE_LEFT;
        } else if (out->x >= 320.0f) {
            out->x += WIDE_RIGHT - 320.0f;
        }
    }
    out->y = sVpTransY - (v->clip[1] * invW * sVpScaleY);
    // Negated so nearer geometry gets the smaller depth under GL_LESS.
    out->z = -(v->clip[2] * invW);
    // Kept so the host layer can restore the homogeneous position and get
    // perspective-correct texturing out of the fixed-function pipeline. The near
    // clip guarantees this is >= GFX_NEAR_W, so it is safe to multiply back by.
    out->w = v->clip[3];
    out->u = v->u;
    out->v = v->v;

    // Fog: the RSP's own formula, ndc_z * mul + ofs, clamped to a byte. The
    // geometry mode gates it — material_set() sets and clears G_FOG per material,
    // and clears it whenever a material wants vertex alpha instead, because the
    // RSP keeps the fog factor in the shade-alpha slot.
    if (sGeometryMode & G_FOG) {
        f32 fog = ((v->clip[2] * invW) * (f32) sFogMul) + (f32) sFogOfs;

        if (fog < 0.0f) {
            fog = 0.0f;
        }
        if (fog > 255.0f) {
            fog = 255.0f;
        }
        out->fog = fog / 255.0f;
    } else {
        out->fog = 0.0f;
    }

    // Run the combiner on this vertex's shade. The texture unit modulates the
    // texel in afterwards, so what comes out here is everything the RDP would
    // have computed *around* the texel: the environment blend, the prim colour,
    // and the prim/vertex alpha that drives every fade in the game.
    // 3D geometry stays on a plain texel * colour modulate, so what it wants is
    // the combiner with the texel taken as white. Shade varies per vertex here, so
    // the texel-blend the 2D path uses is not available: its constant is per-draw
    // state, not per-vertex.
    shade_vertex(v, out);
}

/**
 * Project a triangle and, unless it is marked double-sided, drop it if it is
 * facing away. Winding only exists after the perspective divide, which is why
 * this can't happen back in clip space.
 */
static void push_tri(const GfxVertex *a, const GfxVertex *b, const GfxVertex *c, s32 cull) {
    GfxTriVert v[3];
    f32 area;

    if (sTriVertCount + 3 > GFX_MAX_TRI_VERTS) {
        return;
    }

    project(a, &v[0]);
    project(b, &v[1]);
    project(c, &v[2]);

    if (cull) {
        area = ((v[1].x - v[0].x) * (v[2].y - v[0].y)) - ((v[2].x - v[0].x) * (v[1].y - v[0].y));
        if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
            return;
        }
    }

    sTriVerts[sTriVertCount++] = v[0];
    sTriVerts[sTriVertCount++] = v[1];
    sTriVerts[sTriVertCount++] = v[2];
}

/**
 * Split the edge a->b where it crosses the near plane, at the point where
 * w == GFX_NEAR_W. Colour interpolates linearly in clip space along with the
 * position, which is what the RSP's own clipper does.
 */
static void clip_edge(const GfxVertex *a, const GfxVertex *b, GfxVertex *out) {
    f32 t = (GFX_NEAR_W - a->clip[3]) / (b->clip[3] - a->clip[3]);
    s32 i;

    for (i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + ((b->clip[i] - a->clip[i]) * t);
    }
    out->wide = a->wide;
    out->u = a->u + ((b->u - a->u) * t);
    out->v = a->v + ((b->v - a->v) * t);
    out->r = (u8) (a->r + ((f32) (b->r - a->r) * t));
    out->g = (u8) (a->g + ((f32) (b->g - a->g) * t));
    out->b = (u8) (a->b + ((f32) (b->b - a->b) * t));
    out->a = (u8) (a->a + ((f32) (b->a - a->a) * t));
}

/**
 * Clip a triangle against the near plane (Sutherland-Hodgman on the single
 * w >= GFX_NEAR_W plane), then fan-triangulate whatever polygon survives and
 * emit it. One clipped triangle yields 0, 1 or 2 output triangles.
 */
static void emit_triangle(const GfxVertex *v0, const GfxVertex *v1, const GfxVertex *v2, s32 cull) {
    const GfxVertex *in[3];
    GfxVertex poly[4];
    s32 numOut = 0;
    s32 i;

    in[0] = v0;
    in[1] = v1;
    in[2] = v2;

    for (i = 0; i < 3; i++) {
        const GfxVertex *cur = in[i];
        const GfxVertex *next = in[(i + 1) % 3];
        s32 curIn = cur->clip[3] >= GFX_NEAR_W;
        s32 nextIn = next->clip[3] >= GFX_NEAR_W;

        if (curIn) {
            poly[numOut++] = *cur;
        }
        if (curIn != nextIn) {
            clip_edge(cur, next, &poly[numOut++]);
        }
    }

    if (numOut < 3) {
        return; // entirely behind the near plane
    }

    for (i = 2; i < numOut; i++) {
        push_tri(&poly[0], &poly[i - 1], &poly[i], cull);
    }
}

/**
 * Push the RDP's render mode — the low half of othermode — at the host.
 *
 * DKR never sets this inline: material_set() (src/textures_sprites.c) DMAs in a
 * two-command display list per material, a gsDPSetCombineLERP and a
 * gsDPSetOtherMode, and the render mode is the low word of the latter. So this
 * runs once per G_TRIN, which is once per material batch.
 *
 * The render-mode bits sit at their final positions within othermode-low (the
 * field starts at G_MDSFT_RENDERMODE, which is where AA_EN's 0x8 comes from), so
 * they can be tested against sOtherModeL directly.
 *
 * The blender proper (the GBL_c1/c2 muxes) is still not modelled — everything
 * goes through one fixed src-alpha blend. What is modelled is the part that was
 * doing visible damage: depth compare, depth write, decal offset and alpha
 * compare.
 */
/**
 * G_TF_POINT vs. G_TF_BILERP, from othermode-H. The game point-samples its font
 * and most of its UI art and bilerps the world; filtering everything, as we used
 * to, blurs the text and — because the filter taps reach outside the glyph —
 * drags colour in from whatever the wrap mode puts there.
 *
 * Applies to whatever texture is currently bound, so call this after binding.
 */
static void apply_texture_filter(void) {
    u32 filt = sOtherModeH & (3 << G_MDSFT_TEXTFILT);

    gfx_set_texture_filter(filt == G_TF_POINT);
}

static void apply_render_mode(void) {
    u32 alphaCompare = sOtherModeL & (3 << G_MDSFT_ALPHACOMPARE);
    s32 zEnabled = (sGeometryMode & G_ZBUFFER) != 0;
    f32 ref = 0.0f;

    // Depth needs both halves to agree: the RSP has to be emitting z (geometry
    // mode) and the RDP has to be told to use it (render mode). Clearing G_ZBUFFER
    // is how the game turns depth off for the sky, overlays and its 2D layer.
    gfx_set_depth_test(zEnabled && (sOtherModeL & Z_CMP) != 0);
    gfx_set_depth_write(zEnabled && (sOtherModeL & Z_UPD) != 0);
    gfx_set_depth_offset(zEnabled && (sOtherModeL & ZMODE_DEC) == ZMODE_DEC);

    // CVG_X_ALPHA is how the cutout materials (G_RM_*_TEX_EDGE) get their hard
    // edge: the RDP multiplies coverage by alpha, which on a non-antialiased
    // host is a straight 50% cutout. Otherwise a threshold compare comes from
    // the blend colour's alpha, and G_AC_DITHER — which DKR only uses where a
    // dithered edge is cosmetic — degrades to the same "drop the invisible
    // texels" default as G_AC_NONE.
    if (sOtherModeL & CVG_X_ALPHA) {
        ref = 0.5f;
    } else if (alphaCompare == G_AC_THRESHOLD) {
        ref = sBlendColor[3] / 255.0f;
    }
    gfx_set_alpha_test(ref);
}

// A batch's vertices after projection and shading, each done on first use
// and kept in the form the GPU reads, so a triangle corner is a 32-byte copy
// and two multiplications for its texture coordinates.
typedef struct {
    f32 sx, sy;         // screen position, for the facing and on-screen tests
    GfxGpuVert gpu;
} ProjectedVertex;
static ProjectedVertex sProjected[GFX_MAX_VERTS];
static u32 sProjectedStamp[GFX_MAX_VERTS];
static u32 sProjectedGen;

static const ProjectedVertex *projected_vertex(s32 idx) {
    ProjectedVertex *p = &sProjected[idx];

    if (sProjectedStamp[idx] != sProjectedGen) {
        GfxTriVert t;

        project(&sVerts[idx], &t);
        p->sx = t.x;
        p->sy = t.y;
        p->gpu.x = t.x * t.w;
        p->gpu.y = t.y * t.w;
        p->gpu.z = t.z * t.w;
        p->gpu.w = t.w;
        p->gpu.fog = t.fog;
        p->gpu.r = t.r;
        p->gpu.g = t.g;
        p->gpu.b = t.b;
        p->gpu.a = t.a;
        sProjectedStamp[idx] = sProjectedGen;
    }
    return p;
}

/**
 * G_TRIN — DKR's polygon command. w1 points at an array of Triangles (vertex
 * indices plus UVs); shade each one from its vertex colours.
 */
static void handle_polygon(u32 w0, u32 w1) {
    const Triangle *tris = (const Triangle *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = (params >> 4) + 1;
    s32 texEnabled = params & 1;
    u32 texture = 0;
    f32 invTexW = 0.0f, invTexH = 0.0f;
    GfxGpuVert *fast;   // this batch's triangles, written straight into the frame's vertex buffer
    s32 fastCount;
    s32 i;

    if (tris == NULL || view_hidden()) {
        return;
    }
    PF_BEGIN;

    if (texEnabled) {
#ifdef DL_PROFILE
        u64 texStart = svcGetSystemTick();
#endif
        texture = texture_current();
#ifdef DL_PROFILE
        sPfTicks[PF_TEXTURE] += svcGetSystemTick() - texStart;
        sPfCalls[PF_TEXTURE]++;
#endif
    }
    if (texture != 0) {
        // UVs are S10.5 texel coordinates (32 = one texel), so normalising is a
        // divide by 32 and then by the texture's size.
        invTexW = 1.0f / (32.0f * (f32) sTileWidth);
        invTexH = 1.0f / (32.0f * (f32) sTileHeight);
    }

    // Each G_TRIN is one material batch, so it becomes one draw call.
    sTriVertCount = 0;
    shade_model_prepare();

    sProjectedGen++;
    gProfBatches++;
    if (!sShadeModel.exact) {
        gProfSlowShades++;
    }
    fast = gfx_reserve_tris(count * 3);
    fastCount = 0;

    for (i = 0; i < count; i++) {
        GfxVertex v[3];
        const s32 i0 = tris[i].vi0 % GFX_MAX_VERTS, i1 = tris[i].vi1 % GFX_MAX_VERTS, i2 = tris[i].vi2 % GFX_MAX_VERTS;

        // The usual case, a triangle wholly in front of the near plane: its
        // corners are projected once per batch however many triangles share
        // them, and only the texture coordinates are per triangle.
        if (fast != NULL && sVerts[i0].clip[3] >= GFX_NEAR_W && sVerts[i1].clip[3] >= GFX_NEAR_W &&
            sVerts[i2].clip[3] >= GFX_NEAR_W) {
            const ProjectedVertex *p0 = projected_vertex(i0), *p1 = projected_vertex(i1), *p2 = projected_vertex(i2);
            GfxGpuVert *o;

            if (!(tris[i].flags & BACKFACE_DRAW)) {
                f32 area = ((p1->sx - p0->sx) * (p2->sy - p0->sy)) - ((p2->sx - p0->sx) * (p1->sy - p0->sy));

                if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
                    continue;
                }
            }
            // Wholly off one side of the screen: nothing of it can show.
            if ((p0->sx < WIDE_LEFT && p1->sx < WIDE_LEFT && p2->sx < WIDE_LEFT) ||
                (p0->sx > WIDE_RIGHT && p1->sx > WIDE_RIGHT && p2->sx > WIDE_RIGHT) ||
                (p0->sy < 0.0f && p1->sy < 0.0f && p2->sy < 0.0f) ||
                (p0->sy > (f32) N64_SCREEN_H && p1->sy > (f32) N64_SCREEN_H && p2->sy > (f32) N64_SCREEN_H)) {
                continue;
            }
            o = &fast[fastCount];
            o[0] = p0->gpu;
            o[1] = p1->gpu;
            o[2] = p2->gpu;
            o[0].u = tris[i].uv0.u * invTexW;
            o[0].v = tris[i].uv0.v * invTexH;
            o[1].u = tris[i].uv1.u * invTexW;
            o[1].v = tris[i].uv1.v * invTexH;
            o[2].u = tris[i].uv2.u * invTexW;
            o[2].v = tris[i].uv2.v * invTexH;
            fastCount += 3;
            continue;
        }

        v[0] = sVerts[tris[i].vi0 % GFX_MAX_VERTS];
        v[1] = sVerts[tris[i].vi1 % GFX_MAX_VERTS];
        v[2] = sVerts[tris[i].vi2 % GFX_MAX_VERTS];

        v[0].u = tris[i].uv0.u * invTexW;
        v[0].v = tris[i].uv0.v * invTexH;
        v[1].u = tris[i].uv1.u * invTexW;
        v[1].v = tris[i].uv1.v * invTexH;
        v[2].u = tris[i].uv2.u * invTexW;
        v[2].v = tris[i].uv2.v * invTexH;

        // DKR marks culling per triangle: BACKFACE_DRAW means double-sided.
        emit_triangle(&v[0], &v[1], &v[2], !(tris[i].flags & BACKFACE_DRAW));
    }

    apply_render_mode();
    gfx_set_fog((sGeometryMode & G_FOG) != 0, sFogColor);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_set_texenv_modulate(); // the 2D path leaves the env in blend mode
    set_clip_mode(sVerts[tris[0].vi0 % GFX_MAX_VERTS].wide ? CLIP_WIDE : CLIP_NARROW);
    sProfTriVerts += sTriVertCount + fastCount;
    gfx_commit_tris(fastCount);
    gfx_draw_tris(sTriVerts, sTriVertCount);    // the few that crossed the near plane
    PF_END(PF_POLYGON);
}

// ---------------------------------------------------------------------------
// The colour combiner.
//
// Evaluated on the CPU, per vertex, with TEXEL0/TEXEL1 taken as white — the GL
// texture unit then modulates the real texel in afterwards. That is an
// approximation (the RDP would multiply the texel only into the terms that
// actually name it, where we multiply it into the whole result), but it is exact
// for every combiner that reduces to "texel times a constant", and close for the
// rest. It also needs no shader, no GL_COMBINE and no multitexture, which is the
// same trade the OoT Dreamcast port makes.
//
// The important part is that SHADE is a *real input* now. DKR's directionally-lit
// materials (dRenderSettingsDirectionalLighting, used by objects.c whenever
// directional_lighting_on() is in effect — the intro cutscene, for one) are
//
//     cycle 1: G_CC_BLEND_SHADEALPHA  ->  lerp(PRIM, TEXEL0, SHADE_ALPHA)
//     cycle 2: G_CC_BLENDI_SHADE      ->  lerp(COMBINED, ENV, SHADE)
//
// where the vertex colour is a blend *weight* and the lit colour comes from PRIM
// and ENV. Shading those as texel x vertex-colour, as we did, collapses the model
// towards black — which is exactly what Diddy's plane looked like.
// ---------------------------------------------------------------------------

/**
 * A colour input from one of the 4-bit (a, b) or 3-bit (d) slots. The 4-bit slots
 * encode 0..7 and treat anything from 8 up as zero; the 3-bit d slot puts 1 at 6
 * and zero at 7. Slot 7 of a 4-bit slot is NOISE/K4, neither of which DKR uses,
 * so it lands in the zero default.
 */
static f32 sCcTexel = 1.0f; // the value TEXEL0/TEXEL1 take for this evaluation

static void cc_rgb_in(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    s32 i;

    switch (mux) {
        case G_CCMUX_COMBINED:
            for (i = 0; i < 3; i++) {
                out[i] = comb[i];
            }
            return;
        case G_CCMUX_TEXEL0:
        case G_CCMUX_TEXEL1: // no TEXEL1 yet; it follows TEXEL0
            out[0] = out[1] = out[2] = sCcTexel;
            return;
        case G_CCMUX_1:
            out[0] = out[1] = out[2] = 1.0f;
            return;
        case G_CCMUX_PRIMITIVE:
            for (i = 0; i < 3; i++) {
                out[i] = sPrimColor[i] / 255.0f;
            }
            return;
        case G_CCMUX_SHADE:
            for (i = 0; i < 3; i++) {
                out[i] = shade[i];
            }
            return;
        case G_CCMUX_ENVIRONMENT:
            for (i = 0; i < 3; i++) {
                out[i] = sEnvColor[i] / 255.0f;
            }
            return;
        default:
            out[0] = out[1] = out[2] = 0.0f;
            return;
    }
}

/**
 * A colour input from the 5-bit multiplier (c) slot, which additionally reaches
 * the alpha registers — that is where ENV_ALPHA and SHADE_ALPHA come from, and
 * both are load-bearing for DKR's lighting.
 */
static void cc_rgb_mul(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    f32 scalar;

    switch (mux) {
        case G_CCMUX_COMBINED_ALPHA:
            scalar = comb[3];
            break;
        case G_CCMUX_TEXEL0_ALPHA:
        case G_CCMUX_TEXEL1_ALPHA:
            scalar = sCcTexel;
            break;
        case G_CCMUX_PRIMITIVE_ALPHA:
            scalar = sPrimColor[3] / 255.0f;
            break;
        case G_CCMUX_SHADE_ALPHA:
            scalar = shade[3];
            break;
        case G_CCMUX_ENV_ALPHA:
            scalar = sEnvColor[3] / 255.0f;
            break;
        default:
            // Everything below 7 is a plain colour input; LOD_FRAC and K5 are not
            // used by DKR and fall through cc_rgb_in()'s zero default.
            cc_rgb_in(mux, shade, comb, out);
            return;
    }
    out[0] = out[1] = out[2] = scalar;
}

/** Every alpha slot is 3 bits and they all share one encoding. */
static f32 cc_alpha_in(u32 mux, const f32 shade[4], const f32 comb[4]) {
    switch (mux) {
        case G_ACMUX_COMBINED:
            return comb[3];
        case G_ACMUX_TEXEL0:
        case G_ACMUX_TEXEL1:
            return sCcTexel;
        case G_ACMUX_1:
            return 1.0f;
        case G_ACMUX_PRIMITIVE:
            return sPrimColor[3] / 255.0f;
        case G_ACMUX_SHADE:
            return shade[3];
        case G_ACMUX_ENVIRONMENT:
            return sEnvColor[3] / 255.0f;
        default:
            return 0.0f; // G_ACMUX_0
    }
}

static f32 cc_clamp(f32 v) {
    if (v <= 0.0f) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

static u8 clamp_u8(f32 v) {
    return (u8) (cc_clamp(v) * 255.0f);
}

/** One cycle of (a - b) * c + d, colour and alpha on their own muxes. */
static void cc_cycle(u32 a, u32 b, u32 c, u32 d, u32 aA, u32 bA, u32 cA, u32 dA, const f32 shade[4], const f32 in[4],
                     f32 out[4]) {
    f32 va[3], vb[3], vc[3], vd[3];
    s32 i;

    cc_rgb_in(a, shade, in, va);
    cc_rgb_in(b, shade, in, vb);
    cc_rgb_mul(c, shade, in, vc);
    cc_rgb_in(d, shade, in, vd);

    for (i = 0; i < 3; i++) {
        out[i] = cc_clamp(((va[i] - vb[i]) * vc[i]) + vd[i]);
    }
    out[3] = cc_clamp(((cc_alpha_in(aA, shade, in) - cc_alpha_in(bA, shade, in)) * cc_alpha_in(cA, shade, in)) +
                      cc_alpha_in(dA, shade, in));
}

/**
 * Run the combiner for one shade value. Both cycles, if othermode says so — the
 * directional-lighting materials are two-cycle and the second cycle is where the
 * environment blend lives, so stopping after cycle 0 (as we used to) throws away
 * the half that matters.
 */
static void combiner_run(const f32 shade[4], f32 out[4]) {
    f32 cycle0[4];
    s32 i;

    cc_cycle((sCombineW0 >> 20) & 0xF, (sCombineW1 >> 28) & 0xF, (sCombineW0 >> 15) & 0x1F, (sCombineW1 >> 15) & 0x7,
             (sCombineW0 >> 12) & 0x7, (sCombineW1 >> 12) & 0x7, (sCombineW0 >> 9) & 0x7, (sCombineW1 >> 9) & 0x7,
             shade, shade, cycle0);

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) {
        cc_cycle((sCombineW0 >> 5) & 0xF, (sCombineW1 >> 24) & 0xF, sCombineW0 & 0x1F, (sCombineW1 >> 6) & 0x7,
                 (sCombineW1 >> 21) & 0x7, (sCombineW1 >> 3) & 0x7, (sCombineW1 >> 18) & 0x7, sCombineW1 & 0x7, shade,
                 cycle0, out);
    } else {
        for (i = 0; i < 4; i++) {
            out[i] = cycle0[i];
        }
    }
}

/**
 * Split the combiner into the two halves the fixed-function pipeline can apply:
 * the factor the texel is multiplied by, and the term added to it afterwards.
 *
 * Every combiner DKR uses is affine in TEXEL0 — result = TEXEL0 * K1 + K2 — so
 * running it twice, once with the texel white and once with it black, recovers
 * both halves: K2 is the result with no texel at all, and K1 is what the texel
 * added on top. GL then reproduces it exactly: K1 goes in the vertex colour and
 * modulates the texture, K2 in the secondary colour, which GL_COLOR_SUM adds
 * after texturing.
 *
 * This is what makes the menu's flashing highlight work. The font combiner is
 * G_CC_BLENDT_ENV_ALPHA_A_TxP — lerp(TEXEL0, ENV, ENV_ALPHA) — and font.c puts the
 * highlight colour in ENV and its pulse in ENV's alpha. Folding everything into a
 * single modulated colour, as we used to, computes texel * ENV: at full blend that
 * is texel * white, i.e. the original texel, and the flash silently cancels out.
 *
 * Alpha stays a plain modulate (GL_COLOR_SUM is RGB-only), which is exact for the
 * TEXEL0 * PRIM form DKR's 2D uses everywhere.
 */
static void combiner_eval(const f32 shade[4], u8 lit[4], u8 unlit[3]) {
    f32 on[4];  // texel = white
    f32 off[4]; // texel = black
    s32 i;

    sCcTexel = 1.0f;
    combiner_run(shade, on);
    sCcTexel = 0.0f;
    combiner_run(shade, off);
    sCcTexel = 1.0f;

    for (i = 0; i < 3; i++) {
        lit[i] = clamp_u8(on[i]);
        unlit[i] = clamp_u8(off[i]);
    }
    lit[3] = clamp_u8(on[3]);
}

/**
 * The two colours a rectangle's vertices carry. A rectangle has no shade, so it
 * goes in as white and the combiner is constant across the quad — but it still has
 * to be split into its modulate and add halves, because the font blends the glyph
 * towards ENV and that add is the whole point (see combiner_eval).
 */
static void combiner_color(u8 lit[4], u8 unlit[3]) {
    static const f32 white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Kept like the models of shade_model_prepare: a screen of text is
    // hundreds of rectangles in a few colours.
    static struct {
        CombinerKey key;
        u8 lit[4], unlit[3], valid;
    } cache[COMBINER_CACHE];
    CombinerKey key;
    u32 slot;

    combiner_key(&key);
    slot = combiner_key_slot(&key);
    if (!cache[slot].valid || memcmp(&key, &cache[slot].key, sizeof(key)) != 0) {
        combiner_eval(white, cache[slot].lit, cache[slot].unlit);
        cache[slot].key = key;
        cache[slot].valid = TRUE;
    }
    memcpy(lit, cache[slot].lit, 4);
    memcpy(unlit, cache[slot].unlit, 3);
}

/**
 * Two triangles in screen space, which is all a rectangle is once the RDP is out
 * of the picture. Depth testing is off: the game clears G_ZBUFFER for its 2D and
 * relies on display-list order, and so do we.
 */
static u8 sQuadOnWideCanvas;

static void draw_2d_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, const u8 lit[4],
                         const u8 unlit[3], u32 texture) {
    GfxTriVert q[6];
    s32 i;

    if (view_hidden()) {
        return;
    }
    if (sQuadOnWideCanvas) {
        // A tile of a background laid out for the whole wide screen
        // (handle_texrect): where it says, nothing pulled or cut.
        set_clip_mode(CLIP_FULL);
    } else if (x0 <= 0.5f && x1 >= 319.5f) {
        x0 = WIDE_LEFT;
        x1 = WIDE_RIGHT;
        set_clip_mode(CLIP_FULL);
    } else {
        set_clip_mode(CLIP_RECT);
    }

    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x1; q[2].y = y1; q[2].u = u1; q[2].v = v1;
    q[3].x = x0; q[3].y = y0; q[3].u = u0; q[3].v = v0;
    q[4].x = x1; q[4].y = y1; q[4].u = u1; q[4].v = v1;
    q[5].x = x0; q[5].y = y1; q[5].u = u0; q[5].v = v1;

    for (i = 0; i < 6; i++) {
        q[i].z = 0.0f;
        q[i].w = 1.0f; // already in screen space — nothing to undo
        q[i].fog = 0.0f;
        // Textured: the vertex carries the no-texel end of the combiner and the
        // texture env carries the full-texel end, and GL_BLEND lerps between them
        // by the texel — reproducing the combiner exactly. Untextured: there is no
        // texel to lerp with, so the vertex carries the whole result.
        q[i].r = (texture != 0) ? unlit[0] : lit[0];
        q[i].g = (texture != 0) ? unlit[1] : lit[1];
        q[i].b = (texture != 0) ? unlit[2] : lit[2];
        q[i].a = lit[3];
    }

    if (texture != 0) {
        gfx_set_texenv_blend(lit);
    } else {
        gfx_set_texenv_modulate();
    }

    // The 2D layer is ordered by the display list, not by depth. Everything the
    // last material batch left set has to be undone; the next G_TRIN re-applies
    // its own state through apply_render_mode(), so nothing needs restoring.
    gfx_set_depth_test(FALSE);
    gfx_set_depth_write(FALSE);
    gfx_set_depth_offset(FALSE);
    gfx_set_alpha_test(0.0f);
    gfx_set_fog(FALSE, sFogColor); // the HUD does not sit in the world's haze
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_draw_tris(q, 6);
}

/**
 * G_TEXRECT / G_TEXRECTFLIP — the whole 2D layer: every glyph, HUD sprite and
 * menu image. `w2`/`w3` are the two G_RDPHALF words that follow the opcode and
 * carry the texture coordinates.
 */
static void handle_texrect(u32 w0, u32 w1, u32 w2, u32 w3, s32 flip) {
    // Screen coordinates are 10.2 fixed point, texture coordinates S10.5, and
    // the per-pixel texture steps S5.10.
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    f32 s0 = (f32) (s16) (w2 >> 16) / 32.0f;
    f32 t0 = (f32) (s16) (w2 & 0xFFFF) / 32.0f;
    f32 dsdx = (f32) (s16) (w3 >> 16) / 1024.0f;
    f32 dtdy = (f32) (s16) (w3 & 0xFFFF) / 1024.0f;
    f32 s1, t1;
    u8 lit[4];
    u8 unlit[3];
    u32 texture = texture_current();

    if (texture == 0) {
        return;
    }

    // A flipped rect walks s down the rectangle's height and t across its width.
    if (flip) {
        s1 = s0 + ((y1 - y0) * dsdx);
        t1 = t0 + ((x1 - x0) * dtdy);
    } else {
        s1 = s0 + ((x1 - x0) * dsdx);
        t1 = t0 + ((y1 - y0) * dtdy);
    }

    // The texel coordinates are relative to the image, but the texture we
    // uploaded starts at the tile's upper-left corner.
    s0 -= sTileUls / 4.0f;
    s1 -= sTileUls / 4.0f;
    t0 -= sTileUlt / 4.0f;
    t1 -= sTileUlt / 4.0f;

    combiner_color(lit, unlit);
    // Textured rectangles between PC_STRETCH_MARKs are tiles of a background
    // that the game lays out 400 wide instead of 320 (bgdraw_texture in
    // src/rcp_dkr.c): column 0 of that is the wide screen's left edge.
    // (Rectangle coordinates cannot be negative, hence the shift here.)
    if (sStretchFlat) {
        x0 += WIDE_LEFT;
        x1 += WIDE_LEFT;
        sQuadOnWideCanvas = 1;
    }
    draw_2d_quad(x0, y0, x1, y1, s0 / sTileWidth, t0 / sTileHeight, s1 / sTileWidth, t1 / sTileHeight, lit, unlit,
                 texture);
    sQuadOnWideCanvas = 0;
}

/**
 * G_FILLRECT — the letterbox bars, dialogue-box backgrounds and screen fades.
 */
static void handle_fillrect(u32 w0, u32 w1) {
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    u8 color[4];
    u8 unlit[3] = { 0, 0, 0 };

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_FILL) {
        // Fill mode paints the fill colour flat, and its lower-right corner is
        // inclusive — one pixel further than the same rect in 1-cycle mode. The
        // colour's alpha bit is the framebuffer's coverage bit, not
        // transparency: the rect is opaque whichever way it is set.
        color[0] = sFillColor[0];
        color[1] = sFillColor[1];
        color[2] = sFillColor[2];
        color[3] = 0xFF;
        x1 += 1.0f;
        y1 += 1.0f;
    } else {
        combiner_color(color, unlit);
        if (color[3] == 0) {
            return; // fully transparent — the blender would discard it anyway
        }
    }

    draw_2d_quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, color, unlit, 0);
}

/**
 * Interpret one display list. A `count` above zero means "at most this many
 * commands" — the task's own display list is bounded by its length rather than
 * a G_ENDDL, and G_DMADL likewise DMAs a fixed-size block. Zero means "run
 * until G_ENDDL", which is how nested G_DL lists terminate.
 */
static void run_dl(const Gfx *dl, s32 count, s32 depth) {
    s32 i;

    if (dl == NULL || depth > GFX_MAX_DEPTH) {
        return;
    }

    for (i = 0; count == 0 || i < count; i++) {
        u32 w0 = dl[i].words.w0;
        u32 w1 = dl[i].words.w1;
        u32 cmd = (w0 >> 24) & 0xFF;

        // The immediate-mode opcodes are negative ints in gbi.h (G_ENDDL is
        // G_IMMFIRST-7 = -72), so every case has to be masked to the byte the
        // display list actually holds.
        switch (cmd) {
            case G_MTX: {
                s32 slot = (((w0 >> 16) & 0xFF) >> 6) & 3;
                if (w1 != 0 && slot < 3) {
                    PF_BEGIN;
                    mtx_to_float((const Mtx *) w1, sMatrices[slot]);
                    sMatrixWide[slot] = sMatrices[slot][0][3] != 0.0f || sMatrices[slot][1][3] != 0.0f ||
                                        sMatrices[slot][2][3] != 0.0f;
                    sCurMatrix = slot;
                    PF_END(PF_MATRIX);
                }
                break;
            }
            case (u8) G_MOVEWORD: {
                u32 index = w0 & 0xFF;
                if (index == G_MW_MVPMATRIX) {
                    sCurMatrix = (w1 >> 6) & 3;
                } else if (index == G_MW_BILLBOARD) {
                    sBillboard = (w1 != 0);
                } else if (index == G_MW_FOG) {
                    // gSPFogPosition packs the multiplier in the high half and the
                    // offset in the low half; both are signed.
                    sFogMul = (s16) (w1 >> 16);
                    sFogOfs = (s16) (w1 & 0xFFFF);
                }
                break;
            }
            case G_MOVEMEM: {
                // gSPViewport goes through gDma1p, which packs the parameter at
                // bits 16-23 and the *length* at 0-15 (include/PR/gbi.h). Reading
                // the low byte matched the length, never the parameter, so the
                // viewport was silently ignored: everything rendered at the
                // 320x240 default, which looks right full-screen and falls apart
                // the moment the game asks for a smaller one.
                if (((w0 >> 16) & 0xFF) == G_MV_VIEWPORT && w1 != 0) {
                    const Vp *vp = (const Vp *) w1;

                    sVpOrigScaleX = vp->vp.vscale[0] / 4.0f;
                    sVpOrigScaleY = vp->vp.vscale[1] / 4.0f;
                    sVpOrigTransX = vp->vp.vtrans[0] / 4.0f;
                    sVpOrigTransY = vp->vp.vtrans[1] / 4.0f;
                    apply_viewport();
                }
                break;
            }
            case 0xC0: // G_NOOP: a view or stretch mark, when it carries the tag
                if ((w1 & 0xFFFF0000) == PC_STRETCH_MARK_TAG) {
                    sStretchFlat = (u8) (w1 & 1);
                }
                if ((w1 & 0xFFFF0000) == PC_VIEW_MARK_TAG) {
                    s32 player = w1 & 0xFF;

                    sMarkCount = (w1 >> 8) & 0xFF;
                    // A frame with one view is everybody's, whoever it follows.
                    sMarkPlayer = (player == 0xFF || sMarkCount <= 1) ? -1 : player;
                    if (sLocalView >= 0) {
                        // The scissor the game set is for the split layout.
                        sScisX0 = 0.0f;
                        sScisY0 = 0.0f;
                        sScisX1 = (f32) N64_SCREEN_W;
                        sScisY1 = (f32) N64_SCREEN_H;
                        apply_viewport();
                    }
                }
                break;
            case G_VTX:
                handle_vertex(w0, w1);
                break;
            case G_TRIN:
                handle_polygon(w0, w1);
                break;
            case G_DL:
                run_dl((const Gfx *) w1, 0, depth + 1);
                if (((w0 >> 16) & 0xFF) == G_DL_NOPUSH) {
                    return; // a branch, not a call
                }
                break;
            case G_DMADL:
                run_dl((const Gfx *) w1, (w0 >> 16) & 0xFF, depth + 1);
                break;
            case (u8) G_SETTIMG:
                sTimgAddr = w1;
                break;
            case (u8) G_SETTILE: {
                // Only the tile the triangles actually render with matters.
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    sTileFmt = (w0 >> 21) & 0x7;
                    sTileSiz = (w0 >> 19) & 0x3;
                    sTilePalette = (w1 >> 20) & 0xF;
                    // cmt/cms are 2-bit fields (bit 0 mirror, bit 1 clamp), not
                    // flags. Reading only the low bit picked up mirror and
                    // dropped clamp entirely, so every clamped texture wrapped.
                    sTileCmT = (w1 >> 18) & 0x3;
                    sTileCmS = (w1 >> 8) & 0x3;
                }
                break;
            }
            case (u8) G_SETTILESIZE: {
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    // uls/ult/lrs/lrt are 10.2 fixed point, inclusive bounds.
                    u32 uls = (w0 >> 12) & 0xFFF;
                    u32 ult = w0 & 0xFFF;
                    u32 lrs = (w1 >> 12) & 0xFFF;
                    u32 lrt = w1 & 0xFFF;
                    sTileWidth = ((lrs - uls) >> 2) + 1;
                    sTileHeight = ((lrt - ult) >> 2) + 1;
                    sTileUls = uls;
                    sTileUlt = ult;
                }
                break;
            }
            case (u8) G_LOADBLOCK:
                // Latch the image being loaded: gDPLoadTLUT_pal16 issues its own
                // G_SETTIMG for the palette afterwards, which would otherwise
                // overwrite the texture address before the triangles draw.
                sTexAddr = sTimgAddr;
                // dxt is the low 12 bits. Zero means the RDP applied no odd-row
                // swizzle on the way in, i.e. the asset is already swizzled.
                sTexSwapped = ((w1 & 0xFFF) == 0);
                break;
            case (u8) G_LOADTILE:
                sTexAddr = sTimgAddr;
                sTexSwapped = FALSE; // a tile load is row-by-row, never swizzled
                break;
            case (u8) G_LOADTLUT:
                // The palette is whatever G_SETTIMG last pointed at.
                sTlutAddr = sTimgAddr;
                break;
            case (u8) G_TEXRECT:
            case (u8) G_TEXRECTFLIP: {
                // A texture rectangle is three display-list words: the opcode,
                // then G_RDPHALF_1 and G_RDPHALF_2 carrying the texture
                // coordinates. Skip past them so they are not run as commands.
                if (count != 0 && i + 2 >= count) {
                    return; // truncated — the halves are not there to read
                }
                {
                    PF_BEGIN;
                    handle_texrect(w0, w1, dl[i + 1].words.w1, dl[i + 2].words.w1, cmd == (u8) G_TEXRECTFLIP);
                    PF_END(PF_RECT);
                }
                i += 2;
                break;
            }
            case (u8) G_FILLRECT: {
                PF_BEGIN;
                handle_fillrect(w0, w1);
                PF_END(PF_RECT);
                break;
            }
            case (u8) G_SETSCISSOR: {
                // 10.2 fixed point, lower-right inclusive — a full-screen scissor
                // arrives as (0, 0, 319, 239), hence the +1 to make it exclusive.
                // This is what clips the dialogue box's scrolling text reveal
                // (src/font.c) and, with the viewport, each player's half.
                sScisX0 = ((w0 >> 12) & 0xFFF) / 4.0f;
                sScisY0 = (w0 & 0xFFF) / 4.0f;
                sScisX1 = (((w1 >> 12) & 0xFFF) / 4.0f) + 1.0f;
                sScisY1 = ((w1 & 0xFFF) / 4.0f) + 1.0f;
                if (view_remapped()) {
                    // The game cuts this player's part out of a split
                    // screen; here that part is the whole screen.
                    sScisX0 = 0.0f;
                    sScisY0 = 0.0f;
                    sScisX1 = (f32) N64_SCREEN_W;
                    sScisY1 = (f32) N64_SCREEN_H;
                }
                apply_clip_rect();
                break;
            }
            case (u8) G_SETGEOMETRYMODE:
                sGeometryMode |= w1;
                break;
            case (u8) G_CLEARGEOMETRYMODE:
                sGeometryMode &= ~w1;
                break;
            case (u8) G_SETCOMBINE:
                sCombineW0 = w0;
                sCombineW1 = w1;
                break;
            case (u8) G_SETPRIMCOLOR:
                sPrimColor[0] = (w1 >> 24) & 0xFF;
                sPrimColor[1] = (w1 >> 16) & 0xFF;
                sPrimColor[2] = (w1 >> 8) & 0xFF;
                sPrimColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETFOGCOLOR:
                sFogColor[0] = (w1 >> 24) & 0xFF;
                sFogColor[1] = (w1 >> 16) & 0xFF;
                sFogColor[2] = (w1 >> 8) & 0xFF;
                sFogColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETENVCOLOR:
                sEnvColor[0] = (w1 >> 24) & 0xFF;
                sEnvColor[1] = (w1 >> 16) & 0xFF;
                sEnvColor[2] = (w1 >> 8) & 0xFF;
                sEnvColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETFILLCOLOR: {
                // Two packed RGBA5551 texels, one per 16-bit half of the word;
                // the game always sets both to the same colour.
                u16 texel = w1 & 0xFFFF;
                u32 rgba = rgba16_to_rgba32(texel);
                sFillColor[0] = rgba & 0xFF;
                sFillColor[1] = (rgba >> 8) & 0xFF;
                sFillColor[2] = (rgba >> 16) & 0xFF;
                sFillColor[3] = (rgba >> 24) & 0xFF;
                break;
            }
            case (u8) G_SETOTHERMODE_H:
            case (u8) G_SETOTHERMODE_L: {
                // F3D encodes the field as a shift and a bit count, with the
                // data already sitting at its final position.
                u32 shift = (w0 >> 8) & 0xFF;
                u32 length = w0 & 0xFF;
                u32 mask = (length >= 32) ? 0xFFFFFFFF : (((u32) 1 << length) - 1) << shift;

                if (cmd == (u8) G_SETOTHERMODE_H) {
                    sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
                } else {
                    sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
                }
                break;
            }
            case (u8) G_RDPSETOTHERMODE:
                // Both halves at once: the high word is packed into w0, the low
                // word — render mode, alpha compare, z mode — is all of w1. This
                // is the form every DKR material arrives in (gsDPSetOtherMode).
                sOtherModeH = w0 & 0x00FFFFFF;
                sOtherModeL = w1;
                break;
            case (u8) G_SETBLENDCOLOR:
                // The alpha is the reference value a G_AC_THRESHOLD compare uses.
                sBlendColor[0] = (w1 >> 24) & 0xFF;
                sBlendColor[1] = (w1 >> 16) & 0xFF;
                sBlendColor[2] = (w1 >> 8) & 0xFF;
                sBlendColor[3] = w1 & 0xFF;
                break;
            case (u8) G_ENDDL:
                return;
            default:
                // Everything else is RDP state we do not model (blenders,
                // scissors, texture filters) plus the G_RDPHALF words consumed
                // by G_TEXRECT above.
                break;
        }
    }
}

// linux/audio.c
extern void pc_audio_frame(void);
extern void pc_audio_report(void);

static void apply_pending_invalidations(void) {
    s32 i;

    pc_render_lock();
    if (sPendingInvalidAll) {
        invalidate_range_now(0, 0xFFFFFFFF);
    } else {
        for (i = 0; i < sPendingInvalidCount; i++) {
            invalidate_range_now(sPendingInvalid[i].lo, sPendingInvalid[i].hi);
        }
    }
    sPendingInvalidCount = 0;
    sPendingInvalidAll = FALSE;
    pc_render_unlock();
}

extern void pc_render_post(void *dlBegin, void *dlEnd);

// The game's side of a frame: hand the list to the render thread (waiting
// first for the frame it is still on, as the game waits for the RCP) and
// tick the audio manager.
void pc_gfx_task_submit(void *dlBegin, void *dlEnd) {
    pc_render_post(dlBegin, dlEnd);
    {
        u64 t0 = svcGetSystemTick();

        pc_audio_frame();
        gProfAudioTicks += svcGetSystemTick() - t0;
    }
    pc_audio_report();
}

// The render thread's side: interpret one display list and present it.
void pc_gfx_task_run(void *dlBegin, void *dlEnd) {
    sGfxFrameCount++;

    apply_pending_invalidations();
    gfx_frame_begin();

    sCurMatrix = 0;
    sBillboard = FALSE;
    sVertexBase = 0;
    sTriVertCount = 0;
    sTimgAddr = 0;
    sTexAddr = 0;
    sTexSwapped = 0;
    sTlutAddr = 0;
    sTileWidth = 0;
    sTileHeight = 0;
    sTileUls = 0;
    sTileUlt = 0;
    sOtherModeL = 0;
    sGeometryMode = GFX_GEOMETRY_MODE_INIT;

    // The list sets its own scissor and viewport before it draws anything
    // (src/rcp_dkr.c), but don't inherit last frame's rects until it does.
    sClipMode = CLIP_NARROW;
    sStretchFlat = 0;
    sMarkPlayer = -1;
    sMarkCount = 1;
    sScisX0 = sVpClipX0 = 0.0f;
    sScisY0 = sVpClipY0 = 0.0f;
    sScisX1 = sVpClipX1 = (f32) N64_SCREEN_W;
    sScisY1 = sVpClipY1 = (f32) N64_SCREEN_H;
    gfx_disable_scissor();

    {
        u64 t0 = svcGetSystemTick(), t1, t2;

        run_dl((const Gfx *) dlBegin, (const Gfx *) dlEnd - (const Gfx *) dlBegin, 0);
        t1 = svcGetSystemTick();
#ifdef DL_PROFILE
        pf_report(t1 - t0);
#endif
        gfx_frame_end();
        t2 = svcGetSystemTick();
        // Waiting for the screen's vertical blank is not work; waiting for
        // the GPU is (a frame cannot be handed over faster than that).
        t2 -= gfx_vsync_wait_ticks();
        gRenderCostTicks = t2 - t0;
        gProfDlTicks += t1 - t0;
        gProfPresentTicks += t2 - t1 - gfx_gpu_wait_ticks();
        gProfGpuWaitTicks += gfx_gpu_wait_ticks();
        gProfGpuMs += gfx_gpu_frame_ms();
        if (gfx_gpu_frame_ms() > gProfGpuMaxMs) {
            gProfGpuMaxMs = gfx_gpu_frame_ms();
        }
        gProfTriVerts += sProfTriVerts;
        sProfTriVerts = 0;
    }

    // Tick the audio manager once per frame. On N64 this is the scheduler posting
    // OS_SC_RETRACE_MSG to the audio thread; there is no thread and no scheduler
    // here, so the frame boundary drives it directly. (linux/audio.c)
}

