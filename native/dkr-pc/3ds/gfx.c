// citro3d backend for the F3DDKR interpreter in main.c: the same small
// fixed-function interface as linux/gfx.c (one texture, modulate or
// texel-weighted blend, per-vertex fog, depth, alpha test, scissor), on the
// PICA200's texture-environment stages.
//
// The game draws 320x240; the top screen is 400x240, so the picture sits in
// the middle with 40 pixels of border either side.
#include "gfx.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "dkr_shbin.h"

#define SCREEN_X_OFFSET 40
#define MAX_TEXTURES 2200       // main.c keeps at most 1024 alive; freed ones linger for two frames
#define MAX_FRAME_VERTS 24576
#define MAX_FRAME_DRAWS 4096
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// What the GPU reads: the position already multiplied by w, as glVertex4f
// gets it on the desktop, so the PICA interpolates perspective-correctly.
typedef GfxGpuVert GpuVertex;

static C3D_RenderTarget *sTarget;
static DVLB_s *sShaderDvlb;
static shaderProgram_s sProgram;
static int sProjectionLoc;
static C3D_Mtx sProjection;
// Two vertex buffers, used in turn: the GPU may still be reading last
// frame's while this frame's is written.
static GpuVertex *sVbos[2];
static int sVboIndex;
static GpuVertex *sVbo;
static int sVboUsed;
static int sReady;

static C3D_Tex sTextures[MAX_TEXTURES];
static u8 sTextureUsed[MAX_TEXTURES];
static C3D_Tex sFogRamp;
static u32 *sStage;             // decode staging, grown on demand
static u32 sStageTexels;

// ---------------------------------------------------------------------------
// A frame is made in two steps, so that the processor and the GPU work at the
// same time instead of one after the other.
//
//   1. While the display list is interpreted nothing is sent to the GPU. The
//      vertices go into this frame's vertex buffer and every run of triangles
//      with one state becomes a DrawRecord. The GPU is meanwhile still drawing
//      the frame before.
//   2. gfx_frame_end waits for the GPU to finish that frame, turns the records
//      into GPU commands (a few hundred draws, well under a millisecond) and
//      starts the GPU on them.
//
// Sending commands as the list was interpreted meant waiting for the GPU
// first, so a frame cost the interpreter's time plus the GPU's: on a New 3DS
// the heaviest races took 13 ms of work and still ran at 40-47 fps.
// ---------------------------------------------------------------------------
typedef struct {
    u32 envColor, fogColor;
    s16 scissor[4];
    s16 alphaRef;
    u16 texture;                // handle, 0 = untextured
    u8 pointFilter;
    u8 blendEnv;                // 0 modulate, 1 texel-weighted blend
    u8 fogOn;
    u8 depthTest, depthWrite, depthOffset;
    u8 pad[2];                  // none left to the compiler: records are compared as bytes
} DrawState;

typedef struct {
    DrawState state;
    int first, count;
} DrawRecord;

static DrawState sState;            // as the interpreter last set it
static DrawRecord sDraws[MAX_FRAME_DRAWS];
static int sDrawCount;

// Triangles since the last change of state: one record when the state next
// changes, or at the end of the frame.
static int sBatchFirst, sBatchCount;

// Draw calls since the log's last report (autotest.c).
u32 gProfDraws;
// Draws left out because their clip rectangle was empty (gfx_frame_end).
static u32 sClippedFlat, sClippedWorld, sClippedFrames;

static void flush_batch(void) {
    DrawRecord *d;

    if (sBatchCount <= 0) {
        return;
    }
    // A state that changed and changed back with nothing drawn in between:
    // the same draw goes on.
    if (sDrawCount > 0) {
        d = &sDraws[sDrawCount - 1];
        if (d->first + d->count == sBatchFirst && memcmp(&d->state, &sState, sizeof(DrawState)) == 0) {
            d->count += sBatchCount;
            sBatchCount = 0;
            return;
        }
    }
    if (sDrawCount < MAX_FRAME_DRAWS) {
        d = &sDraws[sDrawCount++];
        d->state = sState;
        d->first = sBatchFirst;
        d->count = sBatchCount;
    }
    sBatchCount = 0;
}

// ---------------------------------------------------------------------------
// The screen's vertical blank, counted here so that the game's pacing
// (pc_retrace_wait in platform.c) and the frame hand-over below both go by
// the screen itself and not by a timer that drifts against it.
// ---------------------------------------------------------------------------
static volatile u32 sVblankCount;
static LightEvent sVblankEvent[2];  // one per waiting thread (GFX_VBLANK_*)

static void on_vblank(void *arg) {
    (void) arg;
    sVblankCount++;
    LightEvent_Signal(&sVblankEvent[0]);
    LightEvent_Signal(&sVblankEvent[1]);
}

void gfx_vblank_hook(void) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, on_vblank, NULL, false);
}

unsigned gfx_vblank_count(void) {
    return sVblankCount;
}

unsigned gfx_vblank_wait(int who, unsigned seen) {
    int tries;

    for (tries = 0; sVblankCount == seen && tries < 2; tries++) {
        if (LightEvent_WaitTimeout(&sVblankEvent[who], 25000000ll) != 0 && sVblankCount == seen) {
            // No blank for longer than a frame: citro3d puts its own callback
            // back when the program returns from being suspended.
            gfx_vblank_hook();
        }
    }
    return sVblankCount;
}

// What the last gfx_frame_end waited for, and the GPU's time for a frame.
static u64 sVsyncWaitTicks, sGpuWaitTicks;
static float sGpuFrameMs;
static u32 sSubmitVblank;

unsigned long long gfx_vsync_wait_ticks(void) {
    return sVsyncWaitTicks;
}

unsigned long long gfx_gpu_wait_ticks(void) {
    return sGpuWaitTicks;
}

float gfx_gpu_frame_ms(void) {
    return sGpuFrameMs;
}

static u32 pack_color(const unsigned char c[4]) {
    return (u32) c[0] | ((u32) c[1] << 8) | ((u32) c[2] << 16) | ((u32) c[3] << 24);
}

static int next_pot(int v) {
    int p = 8;
    while (p < v && p < 1024) {
        p <<= 1;
    }
    return p;
}

// Row-major 0xRRGGBBAA texels (first row on top) into the PICA's 8x8 Morton
// tiles, bottom row first.
static void swizzle32(u32 *dst, const u32 *src, int w, int h) {
    int tilesX = w >> 3;
    int yd, x;

    for (yd = 0; yd < h; yd++) {
        const u32 *row = src + (size_t) (h - 1 - yd) * w;
        u32 *tileRow = dst + (size_t) (yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (x = 0; x < w; x++) {
            tileRow[(x >> 3) * 64 + (my | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = row[x];
        }
    }
}

static u32 *stage_for(u32 texels) {
    if (texels > sStageTexels) {
        free(sStage);
        sStage = malloc(texels * sizeof(u32));
        sStageTexels = sStage != NULL ? texels : 0;
    }
    return sStage;
}

void gfx_window_init(int width, int height, int scale) {
    C3D_AttrInfo *attr;
    u32 *ramp;
    int x, y;

    (void) width; (void) height; (void) scale;
    LightEvent_Init(&sVblankEvent[0], RESET_ONESHOT);
    LightEvent_Init(&sVblankEvent[1], RESET_ONESHOT);
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) {
        printf("GFX: C3D_Init failed\n");
        return;
    }
    sTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sTarget == NULL) {
        printf("GFX: no render target\n");
        return;
    }
    C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    sShaderDvlb = DVLB_ParseFile((u32 *) dkr_shbin, dkr_shbin_size);
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sProjectionLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");

    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);          // v0: position * w
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);          // v1: u, v, fog
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // v2: colour

    sVbos[0] = linearAlloc(sizeof(GpuVertex) * MAX_FRAME_VERTS);
    sVbos[1] = linearAlloc(sizeof(GpuVertex) * MAX_FRAME_VERTS);
    sVbo = sVbos[0];
    if (sVbos[0] == NULL || sVbos[1] == NULL) {
        printf("GFX: no vertex buffers\n");
        return;
    }

    // N64 screen pixels (x right, y down, the 320 columns centred) to the
    // rotated framebuffer, as Mtx_OrthoTilt lays it out. The interpreter's z
    // is -ndc (-1 far .. 1 near after its negation); the PICA wants 0 far ..
    // -1 near.
    Mtx_Zeros(&sProjection);
    sProjection.r[0].y = 2.0f / (0.0f - 240.0f);
    sProjection.r[0].w = (240.0f + 0.0f) / (240.0f - 0.0f);
    sProjection.r[1].x = 2.0f / ((float) -SCREEN_X_OFFSET - (400.0f - SCREEN_X_OFFSET));
    sProjection.r[1].w = ((float) -SCREEN_X_OFFSET + (400.0f - SCREEN_X_OFFSET)) / ((400.0f - SCREEN_X_OFFSET) - (float) -SCREEN_X_OFFSET);
    sProjection.r[2].z = -0.5f;
    sProjection.r[2].w = -0.5f;
    sProjection.r[3].w = 1.0f;

    // The fog factor rides in a second texture coordinate and is looked up
    // in a 0..1 alpha ramp: the PICA's own fog is a function of depth only.
    C3D_TexInit(&sFogRamp, 256, 8, GPU_RGBA8);
    ramp = stage_for(256 * 8);
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 256; x++) {
            ramp[y * 256 + x] = 0xFFFFFF00u | (u32) x;
        }
    }
    swizzle32((u32 *) sFogRamp.data, ramp, 256, 8);
    GSPGPU_FlushDataCache(sFogRamp.data, 256 * 8 * 4);
    C3D_TexSetFilter(&sFogRamp, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFogRamp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    gfx_vblank_hook();          // after C3D_Init, which sets a callback of its own
    sReady = 1;
}

static GPU_TEXTURE_WRAP_PARAM wrap_mode(int cm) {
    if (cm & 0x2) {
        return GPU_CLAMP_TO_EDGE;
    }
    if (cm & 0x1) {
        return GPU_MIRRORED_REPEAT;
    }
    return GPU_REPEAT;
}

unsigned int gfx_create_texture(const void *rgba, int width, int height, int cmS, int cmT) {
    const u8 *src = rgba;
    unsigned int id;
    int pw, ph, x, y;
    u32 *stage;

    if (!sReady || width <= 0 || height <= 0) {
        return 0;
    }
    for (id = 1; id < MAX_TEXTURES; id++) {
        if (!sTextureUsed[id]) {
            break;
        }
    }
    if (id == MAX_TEXTURES) {
        return 0;
    }
    pw = next_pot(width);
    ph = next_pot(height);
    stage = stage_for((u32) pw * ph);
    if (stage == NULL || !C3D_TexInit(&sTextures[id], (u16) pw, (u16) ph, GPU_RGBA8)) {
        return 0;
    }
    // The PICA only has power-of-two textures and the interpreter's
    // coordinates are normalised, so anything else is stretched to fit:
    // wrapping and mirroring then still land on the texture's edges.
    for (y = 0; y < ph; y++) {
        const u8 *row = src + (size_t) (y * height / ph) * width * 4;
        for (x = 0; x < pw; x++) {
            const u8 *p = row + (size_t) (x * width / pw) * 4;
            stage[y * pw + x] = ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
        }
    }
    swizzle32((u32 *) sTextures[id].data, stage, pw, ph);
    GSPGPU_FlushDataCache(sTextures[id].data, (u32) pw * ph * 4);
    C3D_TexSetFilter(&sTextures[id], GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sTextures[id], wrap_mode(cmS), wrap_mode(cmT));
    sTextureUsed[id] = 1;
    return id;
}

void gfx_delete_texture(unsigned int handle) {
    if (handle == 0 || handle >= MAX_TEXTURES || sTextureUsed[handle] != 1) {
        return;
    }
    // Draws recorded this frame may still name it, and the GPU may still be
    // drawing the frame before with it: it is marked here and goes two
    // hand-overs later (gfx_frame_end), when the GPU has finished both.
    flush_batch();
    sTextureUsed[handle] = 2;
    if (sState.texture == handle) {
        sState.texture = 0;
    }
}

void gfx_bind_texture(unsigned int handle) {
    if (handle >= MAX_TEXTURES || sTextureUsed[handle] != 1) {
        handle = 0;
    }
    if (handle != sState.texture) {
        flush_batch();
        sState.texture = (u16) handle;
    }
}

void gfx_set_texture_filter(int point) {
    point = point != 0;
    if (point != sState.pointFilter) {
        flush_batch();
        sState.pointFilter = (u8) point;
    }
}

void gfx_set_fog(int enable, const unsigned char color[4]) {
    u32 c = enable ? pack_color(color) : 0;

    enable = enable != 0;
    if (enable != sState.fogOn || c != sState.fogColor) {
        flush_batch();
        sState.fogOn = (u8) enable;
        sState.fogColor = c;
    }
}

void gfx_set_texenv_blend(const unsigned char color[4]) {
    u32 c = pack_color(color);

    if (!sState.blendEnv || c != sState.envColor) {
        flush_batch();
        sState.blendEnv = 1;
        sState.envColor = c;
    }
}

void gfx_set_texenv_modulate(void) {
    if (sState.blendEnv) {
        flush_batch();
        sState.blendEnv = 0;
        sState.envColor = 0;    // not read in this mode: one value, so that equal states compare equal
    }
}

void gfx_set_depth_test(int enable) {
    enable = enable != 0;
    if (enable != sState.depthTest) {
        flush_batch();
        sState.depthTest = (u8) enable;
    }
}

void gfx_set_depth_write(int enable) {
    enable = enable != 0;
    if (enable != sState.depthWrite) {
        flush_batch();
        sState.depthWrite = (u8) enable;
    }
}

void gfx_set_depth_offset(int enable) {
    enable = enable != 0;
    if (enable != sState.depthOffset) {
        flush_batch();
        sState.depthOffset = (u8) enable;
    }
}

void gfx_set_alpha_test(float ref) {
    int r = (int) (ref * 255.0f);

    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    if (r != sState.alphaRef) {
        flush_batch();
        sState.alphaRef = (s16) r;
    }
}

void gfx_set_scissor(float x0, float y0, float x1, float y1) {
    int ix0 = (int) x0 + SCREEN_X_OFFSET, iy0 = (int) y0, ix1 = (int) x1 + SCREEN_X_OFFSET, iy1 = (int) y1;

    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > 400) ix1 = 400;
    if (iy1 > 240) iy1 = 240;
    if (ix1 <= ix0 || iy1 <= iy0) {
        ix0 = iy0 = ix1 = iy1 = 0;      // nothing shows
    }
    if (ix0 == sState.scissor[0] && iy0 == sState.scissor[1] && ix1 == sState.scissor[2] && iy1 == sState.scissor[3]) {
        return;
    }
    flush_batch();
    sState.scissor[0] = (s16) ix0;
    sState.scissor[1] = (s16) iy0;
    sState.scissor[2] = (s16) ix1;
    sState.scissor[3] = (s16) iy1;
}

void gfx_disable_scissor(void) {
    gfx_set_scissor(-(float) SCREEN_X_OFFSET, 0.0f, 320.0f + SCREEN_X_OFFSET, 240.0f);
}

void gfx_shutdown(void) {
    if (sReady) {
        sReady = 0;
        C3D_Fini();
    }
    gfxExit();
}

// The start of a frame's interpretation. Nothing here touches the GPU, which
// may still be drawing the frame before.
void gfx_frame_begin(void) {
    if (!sReady) {
        return;
    }
    gDiagRenderPhase = DIAG_RENDER_INTERPRET;
    sVboIndex ^= 1;
    sVbo = sVbos[sVboIndex];
    sVboUsed = 0;
    sBatchCount = 0;
    sDrawCount = 0;
    sState.depthTest = sState.depthWrite = 1;
    sState.depthOffset = 0;
    sState.alphaRef = 0;
    gfx_disable_scissor();
}

// One record's state to the GPU: only what differs from the draw before
// (`was` NULL: everything, the first draw of a frame).
static void apply_state(const DrawState *st, const DrawState *was) {
    int texChanged = was == NULL || st->texture != was->texture;

    if (st->texture != 0 && (texChanged || st->pointFilter != was->pointFilter)) {
        GPU_TEXTURE_FILTER_PARAM f = st->pointFilter ? GPU_NEAREST : GPU_LINEAR;

        C3D_TexSetFilter(&sTextures[st->texture], f, f);
        C3D_TexBind(0, &sTextures[st->texture]);
    }
    if (was == NULL || (st->texture == 0) != (was->texture == 0) || st->blendEnv != was->blendEnv ||
        st->envColor != was->envColor || st->fogOn != was->fogOn || st->fogColor != was->fogColor) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);

        C3D_TexEnvInit(env);
        if (st->texture == 0) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        } else if (st->blendEnv) {
            // colour * texel + vertex * (1 - texel); alpha is texel * vertex.
            C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
            C3D_TexEnvColor(env, st->envColor);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        }
        env = C3D_GetTexEnv(1);
        C3D_TexEnvInit(env);
        if (st->fogOn) {
            // fog colour * factor + previous * (1 - factor)
            C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvColor(env, st->fogColor);
            C3D_TexBind(1, &sFogRamp);
        }
        if (was == NULL) {
            int i;

            for (i = 2; i < 6; i++) {
                C3D_TexEnvInit(C3D_GetTexEnv(i));
            }
        }
    }
    if (was == NULL || st->depthTest != was->depthTest || st->depthWrite != was->depthWrite ||
        st->depthOffset != was->depthOffset) {
        // Reversed depth: nearer is larger, so GL_LESS is GEQUAL here, and a
        // decal is nudged towards the viewer by a positive offset.
        // What is drawn in list order (sky, HUD, menus) has the test switched
        // off outright rather than set to "always": the picture is the same
        // and the depth buffer is left alone.
        C3D_DepthTest(st->depthTest != 0, st->depthTest ? GPU_GEQUAL : GPU_ALWAYS,
                      st->depthWrite && st->depthTest ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
        C3D_DepthMap(true, -1.0f, st->depthOffset ? 0.0004f : 0.0f);
    }
    if (was == NULL || st->alphaRef != was->alphaRef) {
        C3D_AlphaTest(true, GPU_GREATER, st->alphaRef);
    }
    if (was == NULL || memcmp(st->scissor, was->scissor, sizeof(st->scissor)) != 0) {
        // Screen y = 0 is framebuffer x = 240 and the screen's left edge is
        // framebuffer y = 400: both axes run backwards.
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32) (240 - st->scissor[3]), (u32) (400 - st->scissor[2]),
                       (u32) (240 - st->scissor[1]), (u32) (400 - st->scissor[0]));
    }
}

GfxGpuVert *gfx_reserve_tris(int count) {
    if (!sReady || count <= 0 || sVboUsed + count > MAX_FRAME_VERTS) {
        return NULL;
    }
    return sVbo + sVboUsed;
}

void gfx_commit_tris(int count) {
    if (count <= 0) {
        return;
    }
    if (sBatchCount == 0) {
        sBatchFirst = sVboUsed;
    }
    sBatchCount += count;
    sVboUsed += count;
}

void gfx_draw_tris(const GfxTriVert *verts, int count) {
    GpuVertex *out;
    int i;

    if (!sReady || count <= 0) {
        return;
    }
    if (sVboUsed + count > MAX_FRAME_VERTS) {
        return;         // the frame's vertex buffer is full: drop the rest
    }
    out = sVbo + sVboUsed;
    for (i = 0; i < count; i++) {
        const GfxTriVert *v = &verts[i];
        float w = v->w;

        out[i].x = v->x * w;
        out[i].y = v->y * w;
        out[i].z = v->z * w;
        out[i].w = w;
        out[i].u = v->u;
        out[i].v = v->v;
        out[i].fog = v->fog;
        out[i].r = v->r;
        out[i].g = v->g;
        out[i].b = v->b;
        out[i].a = v->a;
    }
    if (sBatchCount == 0) {
        sBatchFirst = sVboUsed;
    }
    sBatchCount += count;
    sVboUsed += count;
}

// The frame's records to the GPU, once it has finished the frame before.
void gfx_frame_end(void) {
    C3D_BufInfo buf;
    const DrawState *was = NULL;
    unsigned int id;
    u64 t0, t1;
    int i;

    if (!sReady) {
        return;
    }
    flush_batch();
    gDiagRenderPhase = DIAG_RENDER_END;

    // One frame per refresh of the screen. The game is paced by the vertical
    // blank and so is already in step; this only holds back a frame that
    // follows another within one refresh, which would otherwise be copied to
    // the screen while the one before it was still to be shown.
    t0 = svcGetSystemTick();
    if (gfx_vblank_count() == sSubmitVblank) {
        gfx_vblank_wait(GFX_VBLANK_RENDER, sSubmitVblank);
    }
    t1 = svcGetSystemTick();
    sVsyncWaitTicks = t1 - t0;

    gDiagRenderPhase = DIAG_RENDER_BEGIN;
    if (!C3D_FrameBegin(0)) {   // waits for the GPU to finish the frame before
        return;
    }
    gDiagRenderPhase = DIAG_RENDER_END;
    sGpuWaitTicks = svcGetSystemTick() - t1;
    sGpuFrameMs = C3D_GetDrawingTime();
    sSubmitVblank = gfx_vblank_count();

    // Textures freed while the frame before last was interpreted: the GPU
    // has finished every frame that could name them.
    for (id = 1; id < MAX_TEXTURES; id++) {
        if (sTextureUsed[id] == 3) {
            C3D_TexDelete(&sTextures[id]);
            sTextureUsed[id] = 0;
        } else if (sTextureUsed[id] == 2) {
            sTextureUsed[id] = 3;
        }
    }

    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(sTarget);
    C3D_BindProgram(&sProgram);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjectionLoc, &sProjection);
    BufInfo_Init(&buf);
    BufInfo_Add(&buf, sVbo, sizeof(GpuVertex), 3, 0x210);
    C3D_SetBufInfo(&buf);

    for (i = 0; i < sDrawCount; i++) {
        const DrawRecord *d = &sDraws[i];

        // Nothing to show in: not sent, and counted for the log, because a
        // menu's text that goes missing is first seen here ("clip:").
        if (d->state.scissor[2] <= d->state.scissor[0]) {
            if (d->state.depthTest) {
                sClippedWorld++;
            } else {
                sClippedFlat++;
            }
            continue;
        }
        apply_state(&d->state, was);
        was = &d->state;
        C3D_DrawArrays(GPU_TRIANGLES, d->first, d->count);
    }
    gProfDraws += (u32) sDrawCount;
    if (++sClippedFrames >= 60) {
        if (sClippedFlat != 0 || sClippedWorld != 0) {
            printf("clip: in 60 frames %lu flat draws (text, pictures) and %lu of the world had an empty clip rectangle\n",
                   (unsigned long) sClippedFlat, (unsigned long) sClippedWorld);
        }
        sClippedFrames = sClippedFlat = sClippedWorld = 0;
    }

    // The GPU reads the vertices when the frame's commands run, which is
    // from here on: one cache flush for all of them.
    if (sVboUsed > 0) {
        GSPGPU_FlushDataCache(sVbo, sizeof(GpuVertex) * sVboUsed);
    }
    // GX_CMDLIST_FLUSH: the system flushes the command list itself. Without
    // it citro3d flushes the whole linear heap every frame (1-2 ms on a
    // console); everything else the GPU reads is flushed where it is written
    // (the vertices above, textures in gfx_create_texture).
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
}
