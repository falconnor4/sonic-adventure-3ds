/* the 3DS backend: the engine's services on a New 3DS. the runtime's core
 * keeps the events, arenas and handles; this file answers the rest: the
 * clock, the pad, storage on the SD card, resources and pictures.
 *
 * a picture goes through the PC's scene (engine/pc/scene.cpp), which lights
 * and places each of the game's points as the game would, on the CPU, and
 * sorts the blend list after the opaque. here its corners are carried into
 * clip space on the CPU too, turned to the top screen's rotation, and the
 * PICA200 draws them in batches through citro3d's texture combiner. the
 * PowerVR sorts translucent fragments per pixel, which the PICA200 can't:
 * the scene sorts the blend list by triangle instead.
 *
 * the picture is the game's 640 by 480, squeezed onto the top screen's 400
 * by 240. the game draws 16:9 there, which is near the screen's 5:3 */
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include <3ds.h>
#include <citro3d.h>

#include "pvr.h"
#include "runtime.h"
#include "sa_3ds.h"
#include "sa_pc.h"
#include "sa_sa1.h"
#include "scene.h"
#include "sound.h"
#include "texels.h"

#include "draw.v.shbin.h"
#include "prelude.h"

/* the time the frame takes, which the runner logs (front.c) */
extern "C" uint64_t sa3ds_ticks_begin, sa3ds_ticks_render;
/* where a picture's drawing stands, which front.c's watchdog reports: the
 * records, the scene's end, the wait for the GPU in C3D_FrameBegin, the
 * corners' places, each screen's batches, and C3D_FrameEnd */
extern "C" volatile int sa3ds_draw_step;
volatile int sa3ds_draw_step;
extern "C" volatile int sa3ds_texture_step, sa3ds_audio_busy;
/* the ticks the sound's mixing took since the log last read them (main.c),
 * and the drawing's thread's choice (front.c), which the sound's core
 * follows */
extern "C" volatile uint64_t sa3ds_ticks_audio;
/* the GPU's submission, by part, since the log last read it (main.c): the
 * scene's end, its blend list's sort among it, the corners' places, the
 * cache's flush of them, the batches' state and draws, and C3D_FrameEnd */
extern "C" uint64_t sa3ds_ticks_parts[5];
uint64_t sa3ds_ticks_parts[5];
/* the scene's work on the meshes' points, among the records' time (main.c) */
extern "C" uint64_t sa3ds_ticks_meshes;
uint64_t sa3ds_ticks_meshes;
volatile uint64_t sa3ds_ticks_audio;
extern "C" int sa3ds_drawing_thread;
/* the lock the drawing's thread holds, which a storage call from the game's
 * thread takes too (front.c) */
extern "C" LightLock sa3ds_engine_lock;
/* the game's mode, src/main.h's enum game_mode: 4 and 5 are play in a stage
 * or a field, and 9 a stage the menus picked */
extern "C" int game_mode;
/* settings.txt's "aa 0" draws the top screen without its supersampling, and
 * "fps 30" holds the game at one picture a frame (front.c) */
extern "C" int sa3ds_antialias, sa3ds_fps;

namespace {

/* the top screen, as the GPU's framebuffer holds it: 240 wide and 400 tall,
 * turned a quarter */
constexpr int SCREEN_W = 400;
constexpr int SCREEN_H = 240;
constexpr uint32_t DISPLAY_FLAGS =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
    GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
/* the top screen's antialiasing: each eye's picture drawn twice as fine
 * across the framebuffer's 240, the screen's height, and the display
 * transfer's 2x1 scaling averaging each two pixels into one as it copies the
 * picture to the screen. 2x2 would take some 6 MB of VRAM for the two eyes'
 * colors and depths, all there is; 2x1 takes 3 */
constexpr int AA_SCALE = 2;
constexpr uint32_t AA_DISPLAY_FLAGS =
    (DISPLAY_FLAGS & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X);
/* the corners a picture may send the GPU */
constexpr uint32_t MOST_CORNERS = 3U << 15;
constexpr uint32_t NO_VIEW = UINT32_MAX;
/* the most materials a draw's list may hold */
constexpr uint32_t MOST_MATERIALS = 4096;
/* the nearest a corner may stand. Ninja's drawers clip at z = 1 */
constexpr float NEAR = 1.0F;
/* the stereoscopic screen: at the slider's top, how far an eye's picture
 * moves a point at infinite depth, of the picture's width of 2 in normalized
 * coordinates, some 6 pixels. a point at the convergence's depth stays at
 * the screen, and a nearer one stands out of it. a character stands some 30
 * units from SA1's camera, so the screen's depth is about there */
constexpr float STEREO_SEPARATION = 0.03F;
constexpr float STEREO_CONVERGENCE = 30.0F;
/* the bottom screen, 320 by 240, as its framebuffer holds it turned */
constexpr int BOTTOM_W = 320;
constexpr int BOTTOM_H = 240;
/* in play, the HUD moves to the bottom screen: each sprite the game pins to
 * an edge (SA_SPRITE_HUD) keeps its corner of the picture, the picture's 640
 * by 480, at HUD_SCALE bottom pixels to the picture's. the top screen shows a
 * picture's pixel at 0.625, so they show 1.6 times as large. the rest of the
 * 2D, a tally's or a menu's, stays on the top screen where the game drew it */
constexpr float HUD_SCALE = 1.0F;
/* the sound's buffers on the DSP, and the frames of two samples each holds:
 * four of 1024 frames keep some 90 ms ahead of the DSP */
constexpr uint32_t AUDIO_BUFFERS = 4;
constexpr uint32_t AUDIO_FRAMES = 1024;

/* a corner as the GPU takes it: clip space, uv, and RGBA bytes */
struct Corner {
    float at[4];
    float uv[2];
    uint32_t color;
};

struct Texture {
    C3D_Tex tex;
    bool live = false;
};

struct Resource {
    pc::MeshView mesh;
    const sa_material *materials = nullptr;
    uint32_t material_count = 0;
    uint32_t texture = 0; /* its index in textures, from 1 */
};

struct Backend {
    u64 started = 0;
    bool gpu = false;
    C3D_RenderTarget *target = nullptr; /* the left eye's, and the only */
    C3D_RenderTarget *right = nullptr;
    C3D_RenderTarget *bottom = nullptr;
    int eye_uniform = 0;
    DVLB_s *shader_dvlb = nullptr;
    shaderProgram_s program;
    C3D_AttrInfo attributes;
    C3D_BufInfo buffers;
    Corner *corners = nullptr; /* MOST_CORNERS of them, in linear memory */
    std::vector<Resource> resources;
    /* textures by index - 1, and the indices free for reuse. a texture
     * released goes once the GPU's last picture is drawn */
    std::vector<Texture> textures;
    std::vector<uint32_t> free_textures;
    std::vector<uint32_t> dying_textures;
    pc::Scene scene;
    sa::Arena *picture = nullptr;
    sa_shade shades[256];
    uint32_t shade_count = 0;
    const sa_lights *lights[64];
    uint32_t light_count = 0;
    uint32_t views[SA_VIEWS + 1];
    bool homed = false;
    uint32_t home = 0;
    /* the picture's own view, whose lens projects every draw */
    sa_view lens;
    uint32_t pictures = 0;
    uint32_t refused = 0;
    uint32_t pad_held = 0;
    /* the sound, as the PC's engine renders it, played through the DSP by
     * a thread of its own */
    pc::Sound sound;
    bool sound_made = false;
    Thread audio = nullptr;
    volatile bool audio_quit = false;
    LightEvent audio_due;
    ndspWaveBuf waves[AUDIO_BUFFERS];
    int16_t *wave_data = nullptr;
};

void open_sound(Backend &b);
void close_sound(Backend &b);

Backend &backend(sa_engine &engine)
{
    return **static_cast<Backend **>(engine.backend);
}

void post(sa_engine &engine, uint32_t kind, sa_handle handle, int32_t result)
{
    sa_event event = {};

    event.kind = kind;
    event.handle = handle;
    event.result = result;
    event.tick = engine.tick;
    event.time_ns = sa_clock_ns(&engine);
    sa::event_post(engine, event);
}

Resource *live(sa_engine &engine, sa_handle handle, uint32_t kind)
{
    int32_t slot = engine.resources.find(handle);

    if (slot < 0 || SA_HANDLE_KIND(handle) != kind) {
        return nullptr;
    }
    return &backend(engine).resources[slot];
}

bool texture_of(sa_engine &engine, sa_handle handle, uint32_t &texture)
{
    texture = 0;
    if (handle == SA_NULL) {
        return true;
    }
    Resource *r = live(engine, handle, SA_KIND_TEXTURE);

    if (r == nullptr) {
        return false;
    }
    texture = r->texture;
    return true;
}

const sa_shade *shade_of(const Backend &b, uint32_t id)
{
    for (uint32_t i = b.shade_count; i > 0; i--) {
        if (b.shades[i - 1].shade == id) {
            return &b.shades[i - 1];
        }
    }
    return nullptr;
}

const sa_lights *lights_of(const Backend &b, uint32_t shade)
{
    for (uint32_t i = b.light_count; i > 0; i--) {
        if (b.lights[i - 1]->shade == shade) {
            return b.lights[i - 1];
        }
    }
    return nullptr;
}

/* the view the picture's first draw names becomes the scene's home. a draw
 * whose view the home can't carry goes as the home shows it */
uint32_t take_view(Backend &b, uint32_t id)
{
    uint32_t view = b.views[id];

    if (!b.homed) {
        b.home = view;
        b.homed = true;
        b.scene.home(b.home);
    }
    return b.scene.carries(b.home, view) ? view : b.home;
}

/* a PVR's bytes, a GBIX chunk before its PVRT or not, as RGBA */
bool pvr_file(const void *data, uint32_t size, pc::Image &image)
{
    const auto *from = static_cast<const uint8_t *>(data);
    std::vector<uint8_t> bytes(from, from + size);
    std::string why;
    uint32_t at = 0;

    if (size >= 8 && memcmp(from, "GBIX", 4) == 0) {
        uint32_t length;

        memcpy(&length, from + 4, sizeof length);
        if (length > size - 8) {
            return false;
        }
        at = 8 + length;
    }
    return pc::pvr_decode(bytes, at, image, why);
}

/* a texture's place among the backend's */
uint32_t keep_texture(Backend &b, const Texture &made)
{
    if (!b.free_textures.empty()) {
        uint32_t index = b.free_textures.back();

        b.free_textures.pop_back();
        b.textures[index - 1] = made;
        return index;
    }
    b.textures.push_back(made);
    return (uint32_t)b.textures.size();
}

/* image as a texture of the GPU's, as texels.h makes one: a power of two on
 * each side, sampled from image's nearest texel, in the 16-bit format its
 * alphas choose */
bool make_texture(Backend &b, const pc::Image &image, uint32_t &index)
{
    uint32_t w = texels_side(image.width), h = texels_side(image.height);
    uint32_t format = texels_format16(image.rgba.data(), image.width, image.height);
    static uint32_t columns[1024];
    Texture made;

    if (!C3D_TexInit(&made.tex, (u16)w, (u16)h, (GPU_TEXCOLOR)format)) {
        return false;
    }
    texels_make16(image.rgba.data(), image.width, image.height, format,
                  static_cast<uint16_t *>(made.tex.data), w, h, columns);
    C3D_TexFlush(&made.tex);
    made.live = true;
    index = keep_texture(b, made);
    return true;
}

/* texels the host made ready (SA_FORMAT_GPU), copied as they are */
bool copy_texture(Backend &b, const sa_gpu_texels &head, const uint8_t *texels, uint32_t &index)
{
    Texture made;

    sa3ds_texture_step = 3;
    if (!C3D_TexInit(&made.tex, head.width, head.height, (GPU_TEXCOLOR)head.format)) {
        return false;
    }
    sa3ds_texture_step = 4;
    memcpy(made.tex.data, texels, texels_bytes(head.width, head.height, head.format));
    sa3ds_texture_step = 5;
    C3D_TexFlush(&made.tex);
    sa3ds_texture_step = 2;
    made.live = true;
    index = keep_texture(b, made);
    return true;
}

GPU_BLENDFACTOR blend_factor(uint8_t factor, bool source)
{
    switch (factor) {
    case SA_BLEND_ZERO:
        return GPU_ZERO;
    case SA_BLEND_ONE:
        return GPU_ONE;
    case SA_BLEND_OTHER:
        return source ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case SA_BLEND_INV_OTHER:
        return source ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case SA_BLEND_SRC_ALPHA:
        return GPU_SRC_ALPHA;
    case SA_BLEND_INV_SRC_ALPHA:
        return GPU_ONE_MINUS_SRC_ALPHA;
    case SA_BLEND_DST_ALPHA:
        return GPU_DST_ALPHA;
    default:
        return GPU_ONE_MINUS_DST_ALPHA;
    }
}

GPU_TESTFUNC depth_func(uint8_t depth)
{
    static const GPU_TESTFUNC funcs[8] = {GPU_NEVER,  GPU_LESS,    GPU_EQUAL,
                                          GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL,
                                          GPU_GEQUAL, GPU_ALWAYS};

    return funcs[depth & 7];
}

GPU_TEXTURE_WRAP_PARAM wrap(uint32_t sample, uint32_t clamp, uint32_t flip)
{
    return sample & clamp  ? GPU_CLAMP_TO_EDGE
           : sample & flip ? GPU_MIRRORED_REPEAT
                           : GPU_REPEAT;
}

/* the combiner for a batch: the texel times the point's color, the texel
 * alone under a decal, or the point's color alone. an opaque batch counts
 * the texel's alpha as 1, as the PC's shader does */
void combine(uint32_t flags, bool textured)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    if (!textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR,
                      GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        return;
    }
    bool decal = (flags & pc::BATCH_DECAL) != 0;
    bool opaque = (flags & pc::BATCH_OPAQUE) != 0;

    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
                  GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, decal ? GPU_REPLACE : GPU_MODULATE);
    if (opaque) {
        C3D_TexEnvSrc(env, C3D_Alpha, decal ? GPU_CONSTANT : GPU_PRIMARY_COLOR,
                      GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvColor(env, 0xffffffff);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    } else {
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
                      GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Alpha, decal ? GPU_REPLACE : GPU_MODULATE);
    }
}

/* a corner of the scene's into clip space. a world corner goes through the
 * home's view and the picture's lens: on the screen at center + focal * xy
 * / z, with w the view's z, so the GPU clips at z = NEAR and divides as the
 * PowerVR does. a screen corner lies in pixels with its z. depth is 1/z
 * either way, as the PowerVR keeps it, and the GPU's depth map stores it as
 * given: z / w is -NEAR / z, and the map's scale of -1 makes it NEAR / z.
 * the top screen's framebuffer is turned a quarter, so clip x runs up the
 * picture and clip y to its left. a texture's rows lie from the bottom up,
 * and its t runs the same way, so a picture's v is t as it is */
inline void place(const pc::Vertex &v, bool screen, const float *view,
                  const sa_view &lens, Corner &out)
{
    if (screen) {
        float z = v.at[2] > NEAR ? v.at[2] : NEAR;
        /* products by the reciprocals, where the VFP's divide takes many
         * times as long, each point of each picture */
        float px = v.at[0] * (1.0F / 320.0F) - 1.0F;
        float py = 1.0F - v.at[1] * (1.0F / 240.0F);

        out.at[0] = py;
        out.at[1] = -px;
        out.at[2] = -NEAR / z;
        out.at[3] = 1.0F;
    } else {
        float x = view[0] * v.at[0] + view[1] * v.at[1] + view[2] * v.at[2] + view[3];
        float y = view[4] * v.at[0] + view[5] * v.at[1] + view[6] * v.at[2] + view[7];
        float z = view[8] * v.at[0] + view[9] * v.at[1] + view[10] * v.at[2] + view[11];
        /* the pixel times w */
        float sxw = lens.focal[0] * x + lens.center[0] * z;
        float syw = lens.focal[1] * y + lens.center[1] * z;

        out.at[0] = z - syw * (1.0F / 240.0F);
        out.at[1] = z - sxw * (1.0F / 320.0F);
        out.at[2] = -NEAR;
        out.at[3] = z;
    }
    out.uv[0] = v.uv[0];
    out.uv[1] = v.uv[1];
    /* the scene's RGBA bytes, as the GPU's attribute reads them */
    out.color = v.base;
}

/* a batch as render places it: its corners' first in the GPU's buffer */
struct Placed {
    const pc::Batch *batch;
    uint32_t first, count;
};

bool in_play()
{
    return game_mode == 4 || game_mode == 5 || game_mode == 9;
}

/* a HUD corner on the bottom screen: kept at its corner of the picture,
 * HUD_SCALE as large, in the bottom screen's clip space */
inline void place_bottom(const pc::Vertex &v, bool right, bool lower, Corner &out)
{
    float x = right ? BOTTOM_W - (640.0F - v.at[0]) * HUD_SCALE : v.at[0] * HUD_SCALE;
    float y = lower ? BOTTOM_H - (480.0F - v.at[1]) * HUD_SCALE : v.at[1] * HUD_SCALE;
    float z = v.at[2] > NEAR ? v.at[2] : NEAR;

    out.at[0] = 1.0F - y / (BOTTOM_H / 2.0F);
    out.at[1] = 1.0F - x / (BOTTOM_W / 2.0F);
    out.at[2] = -NEAR / z;
    out.at[3] = 1.0F;
    out.uv[0] = v.uv[0];
    out.uv[1] = v.uv[1];
    out.color = v.base;
}

/* draw the placed batches, under an eye's separation. the screen's own
 * draws, the 2D layer, stand at the screen's depth */
/* the GPU's command buffer, which C3D_Init allocates from linear memory.
 * libctru's GPUCMD_Add stops the program where a frame passes its end, which
 * Emerald Coast did on a console with the 3D slider up, its top screen's
 * batches sent once for each eye. a frame takes the buffer to CMDBUF_FULL at
 * most, and drops the batches past it, which shows as a frame's missing
 * geometry and not a stop */
constexpr uint32_t CMDBUF_BYTES = C3D_DEFAULT_CMDBUF_SIZE * 12;
constexpr float CMDBUF_FULL = 0.95F;
uint32_t dropped_batches;
float cmdbuf_peak;

void draw_batches(Backend &b, const std::vector<Placed> &placed, float separation)
{
    /* the state the last batch set, so a batch sets only what differs: each
     * of citro3d's calls marks state the next draw sends the GPU again */
    float shown = NAN;
    uint32_t bound = UINT32_MAX, sampled = UINT32_MAX, combined = UINT32_MAX;
    uint32_t blended = UINT32_MAX, tested = UINT32_MAX;

    for (const Placed &p : placed) {
        const pc::Batch &batch = *p.batch;
        const pc::State &s = batch.state;
        float shift = s.flags & pc::BATCH_SCREEN ? 0.0F : separation;

        if (shift != shown) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, b.eye_uniform, shift, STEREO_CONVERGENCE,
                          0.0F, 0.0F);
            shown = shift;
        }
        bool textured = s.texture != 0 && s.texture <= b.textures.size() &&
                        b.textures[s.texture - 1].live;

        if (textured && (s.texture != bound || s.sample != sampled)) {
            C3D_Tex *tex = &b.textures[s.texture - 1].tex;
            GPU_TEXTURE_FILTER_PARAM filter =
                (s.sample & pc::SAMPLE_FILTER) == SA_FILTER_POINT ? GPU_NEAREST
                                                                  : GPU_LINEAR;

            C3D_TexSetFilter(tex, filter, filter);
            C3D_TexSetWrap(tex,
                           wrap(s.sample, pc::SAMPLE_CLAMP_U, pc::SAMPLE_FLIP_U),
                           wrap(s.sample, pc::SAMPLE_CLAMP_V, pc::SAMPLE_FLIP_V));
            C3D_TexBind(0, tex);
            bound = s.texture;
            sampled = s.sample;
        }
        uint32_t combine_key = (s.flags & (pc::BATCH_DECAL | pc::BATCH_OPAQUE)) | (textured ? 1U << 31 : 0);

        if (combine_key != combined) {
            combine(s.flags, textured);
            combined = combine_key;
        }
        uint32_t blend_key = s.blend ? 0x10000U | s.src << 8 | s.dst : 0;

        if (blend_key != blended) {
            if (s.blend) {
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                               blend_factor(s.src, true), blend_factor(s.dst, false),
                               blend_factor(s.src, true), blend_factor(s.dst, false));
            } else {
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                               GPU_ONE, GPU_ZERO);
            }
            blended = blend_key;
        }
        uint32_t depth_key = (uint32_t)s.depth << 1 | s.depth_write;

        if (depth_key != tested) {
            C3D_DepthTest(true, depth_func(s.depth),
                          s.depth_write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
            tested = depth_key;
        }
        if (C3D_GetCmdBufUsage() >= CMDBUF_FULL) {
            dropped_batches += (uint32_t)(&placed.back() - &p) + 1;
            return;
        }
        C3D_DrawArrays(GPU_TRIANGLES, (int)p.first, (int)p.count);
    }
}

/* the bottom screen's backdrop under its HUD: a plain gradient, the game's
 * deep blue at the top to a lighter blue at the bottom, as six corners from
 * at. it returns the corner after them */
constexpr uint32_t BACKDROP_TOP = 0xff401c08;    /* RGBA bytes: 08 1c 40 */
constexpr uint32_t BACKDROP_BOTTOM = 0xffa0581c; /* RGBA bytes: 1c 58 a0 */

uint32_t backdrop(Backend &b, uint32_t at)
{
    static const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};

    if (at + 6 > MOST_CORNERS) {
        return at;
    }
    for (int k = 0; k < 6; k++) {
        Corner &c = b.corners[at + k];
        int i = order[k][0], j = order[k][1];

        c.at[0] = j ? -1.0F : 1.0F;
        c.at[1] = i ? -1.0F : 1.0F;
        c.at[2] = -NEAR;
        c.at[3] = 1.0F;
        c.uv[0] = 0.0F;
        c.uv[1] = 0.0F;
        c.color = j ? BACKDROP_BOTTOM : BACKDROP_TOP;
    }
    return at + 6;
}

void draw_backdrop(Backend &b, uint32_t first)
{
    C3D_FVUnifSet(GPU_VERTEX_SHADER, b.eye_uniform, 0.0F, STEREO_CONVERGENCE, 0.0F, 0.0F);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    combine(0, false);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
}

/* draw the scene's batches on the top screen, for each eye */
void render(Backend &b)
{
    const std::vector<pc::Vertex> &vertices = b.scene.vertices();
    const std::vector<pc::Batch> &batches = b.scene.batches();
    pc::PictureUniform u;
    uint32_t back = b.scene.clear_color();
    uint32_t clear = ((back >> 16) & 0xff) << 24 | ((back >> 8) & 0xff) << 16 |
                     (back & 0xff) << 8 | 0xff;

    b.scene.uniform(u);
    uint64_t begin_at = svcGetSystemTick();
    /* no wait for a vblank: the runner paces the game's frames itself */
    sa3ds_draw_step = 2;
    C3D_FrameBegin(0);
    sa3ds_draw_step = 3;
    sa3ds_ticks_begin += svcGetSystemTick() - begin_at;
    uint64_t part_at = svcGetSystemTick();
    /* the GPU has drawn the last picture, so what it read may go */
    for (uint32_t index : b.dying_textures) {
        C3D_TexDelete(&b.textures[index - 1].tex);
        b.textures[index - 1].live = false;
        b.free_textures.push_back(index);
    }
    b.dying_textures.clear();
    /* each corner, placed once for both eyes. in play, the HUD's counters
     * go to the bottom screen instead */
    static std::vector<Placed> top, under;
    uint32_t sent = 0;
    bool play = in_play();

    top.clear();
    under.clear();
    for (const pc::Batch &batch : batches) {
        bool screen = (batch.state.flags & pc::BATCH_SCREEN) != 0;

        if (sent + batch.count > MOST_CORNERS) {
            break;
        }
        if (screen && play && (batch.state.flags & pc::BATCH_SPRITE_2D)) {
            /* the HUD's, to the bottom screen: each triangle keeps the
             * corner of the picture its middle lies nearest */
            for (uint32_t i = 0; i + 2 < batch.count; i += 3) {
                const pc::Vertex *tri = &vertices[batch.first + i];
                float x = (tri[0].at[0] + tri[1].at[0] + tri[2].at[0]) / 3.0F;
                float y = (tri[0].at[1] + tri[1].at[1] + tri[2].at[1]) / 3.0F;

                for (uint32_t k = 0; k < 3; k++) {
                    place_bottom(tri[k], x > 320.0F, y > 240.0F, b.corners[sent + i + k]);
                }
            }
            under.push_back(Placed{&batch, sent, batch.count / 3 * 3});
        } else {
            for (uint32_t i = 0; i < batch.count; i++) {
                place(vertices[batch.first + i], screen, u.view, b.lens,
                      b.corners[sent + i]);
            }
            top.push_back(Placed{&batch, sent, batch.count});
        }
        sent += batch.count;
    }
    uint32_t drop = sent;

    sent = backdrop(b, sent);
    sa3ds_ticks_parts[1] += svcGetSystemTick() - part_at;
    part_at = svcGetSystemTick();
    GSPGPU_FlushDataCache(b.corners, sent * sizeof(Corner));
    sa3ds_ticks_parts[2] += svcGetSystemTick() - part_at;
    part_at = svcGetSystemTick();

    /* the 3D slider: at 0 the left eye's picture alone, which the screen
     * shows to both */
    float slider = osGet3DSliderState();
    int eyes = slider > 0.0F ? 2 : 1;

    for (int eye = 0; eye < eyes; eye++) {
        C3D_RenderTarget *target = eye == 0 ? b.target : b.right;
        /* the left eye's picture moves the far left of the screen's depth,
         * and the right's the far right */
        float separation = (eye == 0 ? -1.0F : 1.0F) * slider * STEREO_SEPARATION;

        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clear, 0);
        C3D_FrameDrawOn(target);
        C3D_BindProgram(&b.program);
        C3D_SetAttrInfo(&b.attributes);
        C3D_SetBufInfo(&b.buffers);
        C3D_DepthMap(true, -1.0F, 0.0F);
        C3D_CullFace(GPU_CULL_NONE);
        sa3ds_draw_step = 4;
        draw_batches(b, top, eyes == 2 ? separation : 0.0F);
    }
    /* the bottom screen: its backdrop, and over it the HUD in play */
    C3D_RenderTargetClear(b.bottom, C3D_CLEAR_ALL, 0x000000ff, 0);
    C3D_FrameDrawOn(b.bottom);
    if (sent > drop) {
        draw_backdrop(b, drop);
    }
    sa3ds_draw_step = 5;
    draw_batches(b, under, 0.0F);
    sa3ds_draw_step = 6;
    cmdbuf_peak = std::max(cmdbuf_peak, C3D_GetCmdBufUsage());
    sa3ds_ticks_parts[3] += svcGetSystemTick() - part_at;
    part_at = svcGetSystemTick();
    C3D_FrameEnd(0);
    sa3ds_ticks_parts[4] += svcGetSystemTick() - part_at;
    sa3ds_draw_step = 7;
}

} // namespace

uint32_t sa::backend_memory(const sa_engine_desc &desc)
{
    (void)desc;
    return sizeof(Backend *);
}

sa_result sa::backend_create(sa_engine &engine)
{
    Backend *b = new (std::nothrow) Backend;

    *static_cast<Backend **>(engine.backend) = b;
    if (b == nullptr) {
        return SA_ERR_FULL;
    }
    b->started = svcGetSystemTick();
    b->resources.resize(engine.desc.resources);
    b->textures.reserve(engine.desc.resources);
    b->scene.open(SA_PC_BLEND_TRIANGLE);
    if (!C3D_Init(CMDBUF_BYTES)) {
        return SA_ERR_UNSUPPORTED;
    }
    b->gpu = true;
    /* the eyes' targets, supersampled where VRAM holds both at their size,
     * and else at the screen's own */
    if (sa3ds_antialias) {
        b->target = C3D_RenderTargetCreate(SCREEN_H * AA_SCALE, SCREEN_W, GPU_RB_RGBA8,
                                           GPU_RB_DEPTH24_STENCIL8);
        b->right = C3D_RenderTargetCreate(SCREEN_H * AA_SCALE, SCREEN_W, GPU_RB_RGBA8,
                                          GPU_RB_DEPTH24_STENCIL8);
        if (b->target == nullptr || b->right == nullptr) {
            if (b->target != nullptr) {
                C3D_RenderTargetDelete(b->target);
            }
            if (b->right != nullptr) {
                C3D_RenderTargetDelete(b->right);
            }
            b->target = b->right = nullptr;
        }
    }
    uint32_t top_flags = b->target != nullptr ? AA_DISPLAY_FLAGS : DISPLAY_FLAGS;

    if (b->target == nullptr) {
        b->target = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W, GPU_RB_RGBA8,
                                           GPU_RB_DEPTH24_STENCIL8);
        b->right = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W, GPU_RB_RGBA8,
                                          GPU_RB_DEPTH24_STENCIL8);
    }
    fprintf(stderr, "engine: the top screen %s\n",
            top_flags == AA_DISPLAY_FLAGS ? "supersampled 2x1" : "without antialiasing");
    C3D_RenderTargetSetOutput(b->target, GFX_TOP, GFX_LEFT, top_flags);
    C3D_RenderTargetSetOutput(b->right, GFX_TOP, GFX_RIGHT, top_flags);
    gfxSet3D(true);
    b->bottom = C3D_RenderTargetCreate(BOTTOM_H, BOTTOM_W, GPU_RB_RGBA8,
                                       GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(b->bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_FLAGS);
    b->shader_dvlb = DVLB_ParseFile((u32 *)draw_v_shbin, draw_v_shbin_size);
    shaderProgramInit(&b->program);
    shaderProgramSetVsh(&b->program, &b->shader_dvlb->DVLE[0]);
    b->eye_uniform = shaderInstanceGetUniformLocation(b->program.vertexShader, "eye");
    AttrInfo_Init(&b->attributes);
    AttrInfo_AddLoader(&b->attributes, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&b->attributes, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&b->attributes, 2, GPU_UNSIGNED_BYTE, 4);
    b->corners = static_cast<Corner *>(linearAlloc(MOST_CORNERS * sizeof(Corner)));
    if (b->corners == nullptr) {
        return SA_ERR_FULL;
    }
    BufInfo_Init(&b->buffers);
    BufInfo_Add(&b->buffers, b->corners, sizeof(Corner), 3, 0x210);
    open_sound(*b);
    return SA_OK;
}

void sa::backend_destroy(sa_engine &engine)
{
    sa_3ds_close(&engine);
}

uint64_t sa_clock_ns(sa_engine *engine)
{
    (void)engine;
    return svcGetSystemTick() * 1000000000ULL / SYSCLOCK_ARM11;
}

sa_result sa_clock_wait(sa_engine *engine, uint64_t time_ns)
{
    uint64_t now = sa_clock_ns(engine);

    while (now < time_ns) {
        svcSleepThread((s64)(time_ns - now));
        now = sa_clock_ns(engine);
    }
    return SA_OK;
}

/* the 3DS's buttons as the Dreamcast pad's: A, B, X and Y where they lie,
 * START, the D-pad, and L and R as the triggers. the circle pad is the
 * stick, and the D-pad drives it too, as the PC's keys do. ZL and ZR, a New
 * 3DS's, are the triggers as well */
sa_result sa_input_sample(sa_engine *engine, uint32_t port, sa_pad *pad)
{
    enum {
        PAD_B = 1 << 1,
        PAD_A = 1 << 2,
        PAD_START = 1 << 3,
        PAD_UP = 1 << 4,
        PAD_DOWN = 1 << 5,
        PAD_LEFT = 1 << 6,
        PAD_RIGHT = 1 << 7,
        PAD_Y = 1 << 9,
        PAD_X = 1 << 10,
        PAD_TR = 1 << 16,
        PAD_TL = 1 << 17
    };
    circlePosition circle;
    u32 keys;
    uint32_t held = 0;

    if (engine == nullptr || pad == nullptr) {
        return SA_ERR_INVALID;
    }
    if (port != 0) {
        return SA_EMPTY;
    }
    Backend &b = backend(*engine);

    hidScanInput();
    keys = hidKeysHeld();
    hidCircleRead(&circle);
    held |= keys & KEY_A ? PAD_A : 0;
    held |= keys & KEY_B ? PAD_B : 0;
    held |= keys & KEY_X ? PAD_X : 0;
    held |= keys & KEY_Y ? PAD_Y : 0;
    held |= keys & KEY_START ? PAD_START : 0;
    held |= keys & KEY_DUP ? PAD_UP : 0;
    held |= keys & KEY_DDOWN ? PAD_DOWN : 0;
    held |= keys & KEY_DLEFT ? PAD_LEFT : 0;
    held |= keys & KEY_DRIGHT ? PAD_RIGHT : 0;
    held |= keys & (KEY_L | KEY_ZL) ? PAD_TL : 0;
    held |= keys & (KEY_R | KEY_ZR) ? PAD_TR : 0;
    memset(pad, 0, sizeof *pad);
    pad->head.kind = SA_REC_PAD;
    pad->head.size = sizeof *pad;
    pad->present = 1;
    /* the circle pad reads about -156 to 156, y up, and the Dreamcast's
     * stick -128 to 127, y down */
    int x = circle.dx * 128 / 150;
    int y = -circle.dy * 128 / 150;

    pad->x1 = (int16_t)(x < -128 ? -128 : x > 127 ? 127 : x);
    pad->y1 = (int16_t)(y < -128 ? -128 : y > 127 ? 127 : y);
    if (held & (PAD_LEFT | PAD_RIGHT)) {
        pad->x1 = held & PAD_LEFT ? -128 : 127;
    }
    if (held & (PAD_UP | PAD_DOWN)) {
        pad->y1 = held & PAD_UP ? -128 : 127;
    }
    pad->l = held & PAD_TL ? 255 : 0;
    pad->r = held & PAD_TR ? 255 : 0;
    pad->on = held;
    pad->press = held & ~b.pad_held;
    pad->release = b.pad_held & ~held;
    b.pad_held = held;
    return SA_OK;
}

/* the 3DS has no rumble */
sa_result sa_input_rumble(sa_engine *engine, uint32_t port, uint16_t low,
                          uint16_t high, uint32_t ms, uint32_t fade_ms)
{
    (void)low;
    (void)high;
    (void)ms;
    (void)fade_ms;
    (void)port;
    return engine == nullptr ? SA_ERR_INVALID : SA_EMPTY;
}

/* storage: a save is the file its name gives, on the SD card, as the PC's
 * is. a request ends in its call, which posts its SA_EVENT_STORAGE before it
 * returns, so the call's result is the request's: a read of a missing file
 * gives SA_ERR_MISSING, and one larger than its buffer SA_ERR_FULL. a write
 * goes to a file beside the save, renamed into place, so a write cut short
 * leaves the save as it was */
namespace {

sa_result finished(sa_engine &engine, int32_t bytes, sa_handle *request)
{
    sa_result result = SA_OK;

    LightLock_Lock(&sa3ds_engine_lock);
    if (sa::event_reserve(engine) != SA_OK) {
        result = SA_ERR_FULL;
    } else if ((*request = engine.resources.take(SA_KIND_REQUEST)) == SA_NULL) {
        engine.event_reserved--;
        result = SA_ERR_FULL;
    } else {
        post(engine, SA_EVENT_STORAGE, *request, bytes);
        engine.resources.give(*request);
    }
    LightLock_Unlock(&sa3ds_engine_lock);
    return result;
}

} // namespace

sa_result sa_storage_read(sa_engine *engine, const char *name, void *buffer,
                          uint32_t bytes, sa_handle *request)
{
    FILE *file;
    long size;

    if (engine == nullptr || name == nullptr || request == nullptr ||
        (buffer == nullptr && bytes != 0)) {
        return SA_ERR_INVALID;
    }
    if (!(file = fopen(name, "rb"))) {
        return errno == ENOENT ? SA_ERR_MISSING : SA_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return SA_ERR_IO;
    }
    if ((unsigned long)size > bytes) {
        fclose(file);
        return SA_ERR_FULL;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        return SA_ERR_IO;
    }
    return finished(*engine, (int32_t)got, request);
}

sa_result sa_storage_write(sa_engine *engine, const char *name,
                           const void *data, uint32_t bytes, sa_handle *request)
{
    std::string fresh;
    FILE *file;

    if (engine == nullptr || name == nullptr || request == nullptr ||
        (data == nullptr && bytes != 0)) {
        return SA_ERR_INVALID;
    }
    fresh = std::string(name) + ".new";
    if (!(file = fopen(fresh.c_str(), "wb"))) {
        return SA_ERR_IO;
    }
    size_t put = fwrite(data, 1, bytes, file);
    bool ok = fclose(file) == 0 && put == bytes;

    if (!ok) {
        remove(fresh.c_str());
        return SA_ERR_IO;
    }
    remove(name);
    if (rename(fresh.c_str(), name) != 0) {
        return SA_ERR_IO;
    }
    return finished(*engine, (int32_t)put, request);
}

/* sound: the game's queues and the adapter run as on the PC, and the PC's
 * engine renders what they ask for, played through the DSP */
sa_result sa_audio_submit(sa_engine *engine, const sa_audio_command *commands,
                          uint32_t count)
{
    if (engine == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    return b.sound_made ? b.sound.submit(commands, count) : SA_ERR_UNSUPPORTED;
}

sa_result sa_audio_load(sa_engine *engine, const sa_audio_bank *bank)
{
    if (engine == nullptr || bank == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    return b.sound_made ? b.sound.load(*bank) : SA_ERR_UNSUPPORTED;
}

sa_result sa_audio_stream(sa_engine *engine, uint32_t stream, const void *data,
                          uint32_t bytes)
{
    if (engine == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    return b.sound_made ? b.sound.stream(stream, data, bytes) : SA_ERR_UNSUPPORTED;
}

extern "C" void sa_3ds_audio_stream_file(sa_engine *engine, uint32_t stream,
                                         FILE *file, uint32_t base, uint32_t bytes)
{
    Backend &b = backend(*engine);

    if (!b.sound_made) {
        fclose(file);
        return;
    }
    b.sound.stream_file(stream, file, base, bytes);
}

namespace {

} // namespace

/* the loudest sample the sound has given since the log last read it */
extern "C" {
volatile uint32_t sa_3ds_audio_peak;
extern uint64_t sa3ds_ticks_render;
uint64_t sa3ds_ticks_begin;
/* the GPU's command buffer's most use, of 1, and the batches it dropped,
 * since the log last read them (main.c) */
float sa3ds_cmdbuf_peak(uint32_t *dropped)
{
    float peak = cmdbuf_peak;

    *dropped = dropped_batches;
    cmdbuf_peak = 0.0F;
    dropped_batches = 0;
    return peak;
}
}

namespace {

void audio_ready(void *data)
{
    LightEvent_Signal(&static_cast<Backend *>(data)->audio_due);
}

/* the sound's thread: each buffer the DSP has played takes the next frames
 * the engine renders. it runs under round-to-nearest in the VFP's RunFast
 * mode, as the engine's calls do */
/* the mixing's guard: each buffer's mixing time against the time it plays.
 * a mix that takes over EFFECTS_FULL of it, averaged over some 8 buffers,
 * can't keep up, and the sound would break up: the DSP's effects, the most
 * of the mixing's cost by far, go, and the voices and the music play on. they
 * come back after EFFECTS_RETRY seconds to try again. each retry that fails
 * at once waits six times as long, to EFFECTS_LONGEST, a failed retry costing
 * a moment of the sound's breaking up */
constexpr float EFFECTS_FULL = 0.9F;
constexpr uint64_t EFFECTS_RETRY = 10;
constexpr uint64_t EFFECTS_LONGEST = 600;
uint32_t effects_bypasses;

void guard_mixing(Backend &b, uint64_t mixing_ticks)
{
    static float load;
    static uint64_t back_at, tried_at, wait = EFFECTS_RETRY;
    static bool retried;
    const float buffer_ticks = (float)AUDIO_FRAMES * SYSCLOCK_ARM11 / SA_PC_SOUND_RATE;
    uint64_t now = svcGetSystemTick();

    load += ((float)mixing_ticks / buffer_ticks - load) / 8.0F;
    if (!b.sound.effects_bypassed()) {
        if (load > EFFECTS_FULL) {
            /* where they failed soon after a retry, they wait longer */
            bool soon = retried && now - tried_at < 5ULL * SYSCLOCK_ARM11;

            wait = soon ? std::min(wait * 6, EFFECTS_LONGEST) : EFFECTS_RETRY;
            b.sound.bypass_effects(true);
            back_at = now + wait * SYSCLOCK_ARM11;
            effects_bypasses++;
            retried = false;
            fprintf(stderr, "engine: the sound's effects off, its mixing at %d%% of its time\n",
                    (int)(load * 100.0F));
        }
    } else if (now >= back_at) {
        b.sound.bypass_effects(false);
        tried_at = now;
        retried = true;
        load = 0.0F;
    }
}

void audio_main(void *data)
{
    Backend &b = *static_cast<Backend *>(data);

    fpscr_write((fpscr_read() & ~(FPSCR_ROUNDING | FPSCR_TRAPS)) | FPSCR_NEAREST |
                FPSCR_RUNFAST);
    while (!b.audio_quit) {
        sa3ds_audio_busy = 1;
        for (ndspWaveBuf &wave : b.waves) {
            if (wave.status != NDSP_WBUF_FREE && wave.status != NDSP_WBUF_DONE) {
                continue;
            }
            uint64_t mixing = svcGetSystemTick();

            b.sound.render(wave.data_pcm16, AUDIO_FRAMES);
            mixing = svcGetSystemTick() - mixing;
            sa3ds_ticks_audio += mixing;
            guard_mixing(b, mixing);
            for (uint32_t i = 0; i < AUDIO_FRAMES * 2; i++) {
                int v = wave.data_pcm16[i] < 0 ? -wave.data_pcm16[i] : wave.data_pcm16[i];

                sa_3ds_audio_peak = v > (int)sa_3ds_audio_peak ? (uint32_t)v
                                                               : sa_3ds_audio_peak;
            }
            DSP_FlushDataCache(wave.data_pcm16, AUDIO_FRAMES * 4);
            ndspChnWaveBufAdd(0, &wave);
        }
        sa3ds_audio_busy = 0;
        LightEvent_Wait(&b.audio_due);
    }
}

/* the DSP and the thread, where the console gives them: ndsp needs the DSP's
 * firmware, which a console keeps at sdmc:/3ds/dspfirm.cdc once dumped.
 * without it the game plays silent */
void open_sound(Backend &b)
{
    if (!b.sound.create()) {
        fprintf(stderr, "engine: no memory for the sound\n");
        return;
    }
    if (R_FAILED(ndspInit())) {
        fprintf(stderr, "engine: no DSP for the sound: sdmc:/3ds/dspfirm.cdc "
                        "holds the DSP's firmware once a dumper writes it\n");
        b.sound.destroy();
        return;
    }
    b.wave_data = static_cast<int16_t *>(
        linearAlloc(AUDIO_BUFFERS * AUDIO_FRAMES * 2 * sizeof(int16_t)));
    if (b.wave_data == nullptr) {
        ndspExit();
        b.sound.destroy();
        return;
    }
    memset(b.wave_data, 0, AUDIO_BUFFERS * AUDIO_FRAMES * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)SA_PC_SOUND_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    memset(b.waves, 0, sizeof b.waves);
    for (uint32_t i = 0; i < AUDIO_BUFFERS; i++) {
        b.waves[i].data_pcm16 = b.wave_data + i * AUDIO_FRAMES * 2;
        b.waves[i].nsamples = AUDIO_FRAMES;
        b.waves[i].status = NDSP_WBUF_FREE;
    }
    LightEvent_Init(&b.audio_due, RESET_ONESHOT);
    ndspSetCallback(audio_ready, &b);
    b.sound_made = true;
    /* the New 3DS's third core, which the game's thread leaves free, where
     * the game's thread draws. where the drawing's thread takes that core,
     * the system core, at the most of its time an application may have: on
     * the third core the sound, above the drawing's thread and mixing nearly
     * all the time, left the drawing a record a second, and Station Square's
     * load never ended. else the game's own core */
    s32 priority = 0x30;
    bool system_first = sa3ds_drawing_thread != 0;

    int core = -1;

    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    b.audio = nullptr;
    if (system_first && R_SUCCEEDED(APT_SetAppCpuTimeLimit(80)) &&
        (b.audio = threadCreate(audio_main, &b, 32 * 1024, priority - 1, 1, false)) != nullptr) {
        core = 1;
    }
    if (b.audio == nullptr &&
        (b.audio = threadCreate(audio_main, &b, 32 * 1024, priority - 1, 2, false)) != nullptr) {
        core = 2;
    }
    if (b.audio == nullptr && R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)) &&
        (b.audio = threadCreate(audio_main, &b, 32 * 1024, priority - 1, 1, false)) != nullptr) {
        core = 1;
    }
    if (b.audio == nullptr &&
        (b.audio = threadCreate(audio_main, &b, 32 * 1024, priority - 1, -2, false)) != nullptr) {
        core = 0;
    }
    fprintf(stderr, "engine: the sound's mixing on core %d\n", core);
    if (b.audio == nullptr) {
        fprintf(stderr, "engine: no thread for the sound\n");
        ndspSetCallback(nullptr, nullptr);
        ndspExit();
        linearFree(b.wave_data);
        b.sound.destroy();
        b.sound_made = false;
    }
}

void close_sound(Backend &b)
{
    if (!b.sound_made) {
        return;
    }
    b.audio_quit = true;
    LightEvent_Signal(&b.audio_due);
    threadJoin(b.audio, U64_MAX);
    threadFree(b.audio);
    ndspChnWaveBufClear(0);
    ndspSetCallback(nullptr, nullptr);
    ndspExit();
    linearFree(b.wave_data);
    b.sound.destroy();
    b.sound_made = false;
}

} // namespace

sa_result sa_resource_create(sa_engine *engine, const sa_resource_desc *desc,
                             sa_handle *resource)
{
    if (engine == nullptr || desc == nullptr || resource == nullptr ||
        (desc->data == nullptr && desc->bytes != 0)) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    Resource made;
    pc::Image image;
    const uint8_t *gpu = nullptr;

    switch (desc->format) {
    case SA_FORMAT_MESH:
        if (desc->kind != SA_KIND_MESH ||
            !pc::mesh_view(desc->data, desc->bytes, made.mesh)) {
            return SA_ERR_INVALID;
        }
        break;
    case SA_FORMAT_PIXELS: {
        sa_pixels size;

        if (desc->kind != SA_KIND_TEXTURE || desc->bytes < sizeof size) {
            return SA_ERR_INVALID;
        }
        memcpy(&size, desc->data, sizeof size);
        if (size.width == 0 || size.height == 0 || size.width > 4096 ||
            size.height > 4096 ||
            desc->bytes < sizeof size + (size.width * size.height * 4)) {
            return SA_ERR_INVALID;
        }
        const auto *rows = static_cast<const uint8_t *>(desc->data) + sizeof size;

        image.width = size.width;
        image.height = size.height;
        image.rgba.assign(rows, rows + (size.width * size.height * 4));
        break;
    }
    case SA_FORMAT_PVR:
        if (desc->kind != SA_KIND_TEXTURE ||
            !pvr_file(desc->data, desc->bytes, image)) {
            return SA_ERR_INVALID;
        }
        break;
    case SA_FORMAT_GPU: {
        sa_gpu_texels head;

        if (desc->kind != SA_KIND_TEXTURE || desc->bytes < sizeof head) {
            return SA_ERR_INVALID;
        }
        memcpy(&head, desc->data, sizeof head);
        if (head.width < 8 || head.height < 8 || head.width > 1024 || head.height > 1024 ||
            (head.width & (head.width - 1)) || (head.height & (head.height - 1)) ||
            desc->bytes < sizeof head + texels_bytes(head.width, head.height, head.format)) {
            return SA_ERR_INVALID;
        }
        gpu = static_cast<const uint8_t *>(desc->data);
        break;
    }
    case SA_FORMAT_MATERIALS:
        if (desc->kind != SA_KIND_MATERIAL ||
            desc->bytes % sizeof(sa_material) != 0 ||
            desc->bytes / sizeof(sa_material) > MOST_MATERIALS) {
            return SA_ERR_INVALID;
        }
        made.materials = static_cast<const sa_material *>(desc->data);
        made.material_count = desc->bytes / sizeof(sa_material);
        break;
    default:
        return SA_ERR_INVALID;
    }
    /* the release's retirement takes the event reserved here */
    sa_result result = sa::event_reserve(*engine);

    if (result != SA_OK) {
        return result;
    }
    sa_handle handle = engine->resources.take(desc->kind);

    if (handle == SA_NULL) {
        engine->event_reserved--;
        return SA_ERR_FULL;
    }
    sa_gpu_texels head{};

    if (gpu != nullptr) {
        memcpy(&head, gpu, sizeof head);
    }
    if (desc->kind == SA_KIND_TEXTURE &&
        !(gpu != nullptr ? copy_texture(b, head, gpu + sizeof head, made.texture)
                         : make_texture(b, image, made.texture))) {
        fprintf(stderr, "engine: no room for a %lux%lu texture\n",
                (unsigned long)(gpu ? head.width : image.width),
                (unsigned long)(gpu ? head.height : image.height));
        engine->resources.give(handle);
        engine->event_reserved--;
        return SA_ERR_FULL;
    }
    b.resources[engine->resources.find(handle)] = made;
    *resource = handle;
    return SA_OK;
}

/* a resource's bytes are read during the calls that name it, into the
 * scene's corners, so it retires as it is released. a texture's own copy
 * goes once the GPU has drawn the last picture that may read it */
sa_result sa_resource_release(sa_engine *engine, sa_handle resource)
{
    if (engine == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    uint32_t kind = SA_HANDLE_KIND(resource);
    int32_t slot = engine->resources.find(resource);

    if (slot < 0 || (kind != SA_KIND_MESH && kind != SA_KIND_TEXTURE &&
                     kind != SA_KIND_MATERIAL)) {
        return SA_ERR_HANDLE;
    }
    if (b.resources[slot].texture != 0) {
        b.dying_textures.push_back(b.resources[slot].texture);
    }
    b.resources[slot] = Resource{};
    engine->resources.give(resource);
    post(*engine, SA_EVENT_RETIRED, resource, 0);
    return SA_OK;
}

/* every resource has retired as it was released, and the GPU draws no
 * picture past its own end */
sa_result sa_pc_retire(sa_engine *engine)
{
    return engine == nullptr ? SA_ERR_INVALID : SA_OK;
}

sa_result sa_present_begin(sa_engine *engine, const sa_picture *picture)
{
    if (engine == nullptr || picture == nullptr ||
        backend(*engine).picture != nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    sa::Arena &arena = engine->pictures[0];
    sa_result result = sa::event_reserve(*engine);

    if (result != SA_OK) {
        return result;
    }
    arena.reset();
    b.shade_count = 0;
    b.light_count = 0;
    b.picture = &arena;
    b.views[0] = 0;
    for (uint32_t i = 1; i <= SA_VIEWS; i++) {
        b.views[i] = NO_VIEW;
    }
    b.homed = false;
    b.lens = picture->view;
    b.scene.begin(*picture);
    return SA_OK;
}

sa_result sa_present_fog(sa_engine *engine, const sa_fog *fog)
{
    if (engine == nullptr || fog == nullptr || backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    backend(*engine).scene.fog(*fog);
    return SA_OK;
}

sa_result sa_present_palettes(sa_engine *engine, const sa_palettes *palettes)
{
    if (engine == nullptr || palettes == nullptr ||
        backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    void *copy = b.picture->take(sizeof palettes->words, 4);

    if (copy == nullptr) {
        return SA_ERR_FULL;
    }
    memcpy(copy, palettes->words, sizeof palettes->words);
    b.scene.palettes(static_cast<const uint32_t *>(copy));
    return SA_OK;
}

sa_result sa_present_copy(sa_engine *engine, const void *data, uint32_t bytes,
                          uint32_t *payload)
{
    if (engine == nullptr || payload == nullptr ||
        (data == nullptr && bytes != 0) || backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    void *copy = b.picture->take(bytes, 4);

    if (copy == nullptr) {
        return SA_ERR_FULL;
    }
    memcpy(copy, data, bytes);
    *payload = static_cast<uint32_t>(static_cast<uint8_t *>(copy) -
                                     static_cast<uint8_t *>(b.picture->at(0)));
    return SA_OK;
}

sa_result sa_present_shade(sa_engine *engine, const sa_shade *shade)
{
    if (engine == nullptr || shade == nullptr || backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    /* the oldest shade gives way where the store is full: a later one with
     * the same id takes its place anyway */
    if (b.shade_count == sizeof b.shades / sizeof b.shades[0]) {
        memmove(b.shades, b.shades + 1, sizeof b.shades - sizeof b.shades[0]);
        b.shade_count--;
    }
    b.shades[b.shade_count++] = *shade;
    return SA_OK;
}

sa_result sa_present_lights(sa_engine *engine, const sa_lights *lights)
{
    if (engine == nullptr || lights == nullptr || backend(*engine).picture == nullptr ||
        lights->count > SA_LIGHTS) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    void *copy = b.picture->take(sizeof *lights, 4);

    if (copy == nullptr) {
        return SA_ERR_FULL;
    }
    memcpy(copy, lights, sizeof *lights);
    if (b.light_count == sizeof b.lights / sizeof b.lights[0]) {
        memmove(b.lights, b.lights + 1, sizeof b.lights - sizeof b.lights[0]);
        b.light_count--;
    }
    b.lights[b.light_count++] = static_cast<const sa_lights *>(copy);
    return SA_OK;
}

sa_result sa_present_view(sa_engine *engine, const sa_picture_view *view)
{
    if (engine == nullptr || view == nullptr || backend(*engine).picture == nullptr ||
        view->view == 0 || view->view > SA_VIEWS) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    if (b.views[view->view] != NO_VIEW) {
        return SA_ERR_INVALID;
    }
    b.views[view->view] = b.scene.view(view->value);
    return SA_OK;
}

sa_result sa_present_draw(sa_engine *engine, const sa_draw *draws, uint32_t count)
{
    if (engine == nullptr || (draws == nullptr && count != 0) ||
        backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    static uint32_t textures[MOST_MATERIALS];

    for (uint32_t i = 0; i < count; i++) {
        const sa_draw &d = draws[i];
        pc::MeshView mesh;

        if (d.flags & SA_DRAW_OUT) {
            continue;
        }
        if (d.mesh != SA_NULL) {
            Resource *r = live(*engine, d.mesh, SA_KIND_MESH);

            if (r == nullptr) {
                b.refused++;
                continue;
            }
            mesh = r->mesh;
        } else {
            uint32_t used = b.picture->used();

            if (d.payload >= used || d.payload % 4 != 0 ||
                !pc::mesh_view(b.picture->at(d.payload), used - d.payload, mesh)) {
                b.refused++;
                continue;
            }
        }
        Resource *m = live(*engine, d.materials, SA_KIND_MATERIAL);
        const sa_shade *shade = shade_of(b, d.shade);
        bool named = d.view <= SA_VIEWS && b.views[d.view] != NO_VIEW;

        if (m == nullptr || shade == nullptr || !named ||
            m->material_count < mesh.materials ||
            (shade->mode == SA_SHADE_PALETTE && !b.scene.has_palettes())) {
            b.refused++;
            continue;
        }
        const sa_lights *lights = nullptr;

        if (shade->mode == SA_SHADE_LIGHTS &&
            (lights = lights_of(b, d.shade)) == nullptr) {
            b.refused++;
            continue;
        }
        bool ok = true;

        for (uint32_t k = 0; k < m->material_count && k < MOST_MATERIALS; k++) {
            ok = ok && texture_of(*engine, m->materials[k].texture, textures[k]);
        }
        if (!ok || !b.scene.room(mesh.triangles)) {
            b.refused++;
            continue;
        }
        uint32_t view = take_view(b, d.view);

        uint64_t drawn = svcGetSystemTick();

        b.scene.draw(d, mesh, m->materials, textures, *shade, lights, view);
        sa3ds_ticks_meshes += svcGetSystemTick() - drawn;
    }
    return SA_OK;
}

sa_result sa_present_sprite(sa_engine *engine, const sa_sprite *sprites, uint32_t count)
{
    if (engine == nullptr || (sprites == nullptr && count != 0) ||
        backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    for (uint32_t i = 0; i < count; i++) {
        const sa_sprite &s = sprites[i];
        uint32_t texture;
        bool screen = (s.flags & SA_SPRITE_SCREEN) != 0;
        bool named = s.view <= SA_VIEWS && b.views[s.view] != NO_VIEW;

        if (!texture_of(*engine, s.texture, texture) || !named || !b.scene.room(2)) {
            b.refused++;
            continue;
        }
        b.scene.sprite(s, texture, screen ? 0 : take_view(b, s.view));
    }
    return SA_OK;
}

sa_result sa_present_triangles(sa_engine *engine, const sa_triangles *triangles,
                               const sa_point *points)
{
    if (engine == nullptr || triangles == nullptr || points == nullptr ||
        backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);
    const sa_triangles &t = *triangles;
    uint32_t texture;
    bool screen = (t.flags & SA_TRIANGLES_SCREEN) != 0;
    bool named = t.view <= SA_VIEWS && b.views[t.view] != NO_VIEW;

    if (!texture_of(*engine, t.texture, texture) || !named ||
        !b.scene.room(t.form == SA_TRIANGLES_LINES ? 2U * t.count : t.count)) {
        b.refused++;
        return SA_OK;
    }
    b.scene.triangles(t, points, texture, screen ? 0 : take_view(b, t.view));
    return SA_OK;
}

sa_result sa_present_end(sa_engine *engine)
{
    if (engine == nullptr || backend(*engine).picture == nullptr) {
        return SA_ERR_INVALID;
    }
    Backend &b = backend(*engine);

    uint64_t began = svcGetSystemTick();

    sa3ds_draw_step = 1;
    uint64_t ended = svcGetSystemTick();

    b.scene.finish();
    sa3ds_ticks_parts[0] += svcGetSystemTick() - ended;
    render(b);
    sa3ds_draw_step = 0;
    sa3ds_ticks_render += svcGetSystemTick() - began;
    b.picture = nullptr;
    b.pictures++;
    post(*engine, SA_EVENT_PRESENTED, SA_NULL, 0);
    return SA_OK;
}

sa_result sa_present_ended(sa_engine *engine, uint32_t instance)
{
    (void)instance;
    return engine == nullptr ? SA_ERR_INVALID : SA_OK;
}

/* whether the next frame may split into two pictures. a split adds a
 * picture's drawing to the frame and nothing to the game's logic, so where a
 * frame's work and a second drawing don't fit in its two vsyncs, 60 would
 * slow the game itself, its logic and its sound with it. the game asks once a
 * frame, and this measures the frame since: its busy time, all but the waits
 * for a vsync, and its drawing, the client's records through the GPU's
 * submission (front.c). at 30 it grants 60 where the busy time and
 * SIXTY_COST times the drawing come under SIXTY_ENTER of the frame: in
 * Emerald Coast a split frame's busy time grew by some twice the drawing a
 * whole frame took, the split's second picture costing more than its first.
 * at 60 it falls back where the busy time passes SIXTY_LEAVE. each is an
 * average over some 8 frames, and a change holds SIXTY_HOLD frames, a fall
 * back from 60 SIXTY_RETRY, so a passing spike moves neither and a frame
 * that can't hold 60 tries it seldom */
constexpr float SIXTY_ENTER = 0.85F;
constexpr float SIXTY_LEAVE = 0.95F;
constexpr float SIXTY_COST = 2.0F;
constexpr uint32_t SIXTY_HOLD = 30;
constexpr uint32_t SIXTY_RETRY = 300;
constexpr uint32_t SIXTY_WARM = 8;

bool sixty()
{
    extern uint64_t sa3ds_ticks_give_ever, sa3ds_ticks_wait_ever;
    static uint64_t last_tick, last_give, last_wait;
    static float busy_avg, draw_avg;
    static bool split;
    static uint32_t held, measured;
    const float frame = 2.0F * SYSCLOCK_ARM11 / 59.94F;
    uint64_t now = svcGetSystemTick();
    uint64_t give = sa3ds_ticks_give_ever, wait = sa3ds_ticks_wait_ever;

    if (last_tick != 0) {
        float all = (float)(now - last_tick);
        float busy = all - (float)(wait - last_wait);
        float draw = (float)(give - last_give);

        /* a load's frame, seconds long, says nothing of play's */
        if (all < 4.0F * frame) {
            busy_avg += (busy - busy_avg) / 8.0F;
            draw_avg += (draw - draw_avg) / 8.0F;
            measured++;
        }
    }
    last_tick = now;
    last_give = give;
    last_wait = wait;
    if (held > 0 || measured < SIXTY_WARM) {
        held -= held > 0;
        return split;
    }
    /* on one thread a frame's drawing adds to its busy time. on the drawing's
     * thread the two run side by side, the game's thread waiting for the
     * drawing only where it lags, so a frame fits where the larger fits */
    bool wanted;

    if (sa3ds_drawing_thread) {
        wanted = split ? std::max(busy_avg, draw_avg) < SIXTY_LEAVE * frame
                       : std::max(busy_avg, SIXTY_COST * draw_avg) < SIXTY_ENTER * frame;
    } else {
        wanted = split ? busy_avg < SIXTY_LEAVE * frame
                       : busy_avg + SIXTY_COST * draw_avg < SIXTY_ENTER * frame;
    }

    if (wanted != split) {
        split = wanted;
        held = split ? SIXTY_HOLD : SIXTY_RETRY;
        fprintf(stderr, "engine: %d fps, a frame busy %.1f ms and drawing %.1f\n", split ? 60 : 30,
                busy_avg * 1000.0F / SYSCLOCK_ARM11, draw_avg * 1000.0F / SYSCLOCK_ARM11);
    }
    return split;
}

sa_result sa_present_mode(sa_engine *engine, const sa_mode *asked, sa_mode *applied)
{
    if (engine == nullptr || asked == nullptr || applied == nullptr) {
        return SA_ERR_INVALID;
    }
    *applied = *asked;
    /* the game asks 60 in play, where its 30 frames a second each split into
     * two pictures, its logic still at 30. settings.txt's "fps 30" holds
     * every frame at one, and else sixty() grants 60 only where a frame fits */
    applied->rate = sa3ds_fps == 60 && asked->rate == 60 && sixty() ? 60 : 30;
    applied->held = asked->rate != applied->rate ? 'r' : 0;
    return SA_OK;
}

sa_result sa_present_window(sa_engine *engine, uint32_t aspect)
{
    (void)aspect;
    return engine == nullptr ? SA_ERR_INVALID : SA_OK;
}

/* a scene the game gives whole, as an act loads: the 3DS keeps none, so each
 * call answers SA_OK and keeps nothing */
sa_result sa_scene_begin(sa_engine *engine, const sa_scene *scene)
{
    return engine == nullptr || scene == nullptr ? SA_ERR_INVALID : SA_OK;
}

sa_result sa_scene_shade(sa_engine *engine, const sa_shade *shade)
{
    return engine == nullptr || shade == nullptr ? SA_ERR_INVALID : SA_OK;
}

sa_result sa_scene_part(sa_engine *engine, const sa_draw *parts, uint32_t count)
{
    return engine == nullptr || (parts == nullptr && count != 0) ? SA_ERR_INVALID
                                                                 : SA_OK;
}

sa_result sa_scene_end(sa_engine *engine)
{
    return engine == nullptr ? SA_ERR_INVALID : SA_OK;
}

sa_result sa_scene_ended(sa_engine *engine, uint32_t scene)
{
    (void)scene;
    return engine == nullptr ? SA_ERR_INVALID : SA_OK;
}

sa_result sa_scene_lighting(sa_engine *engine, const sa_lighting *lighting)
{
    return engine == nullptr || lighting == nullptr ? SA_ERR_INVALID : SA_OK;
}

int sa_3ds_sound(sa_engine *engine)
{
    return engine != nullptr && backend(*engine).sound_made;
}

int sa_3ds_wide(sa_engine *engine)
{
    (void)engine;
    return 1;
}

void sa_3ds_close(sa_engine *engine)
{
    if (engine == nullptr || *static_cast<Backend **>(engine->backend) == nullptr) {
        return;
    }
    Backend &b = backend(*engine);

    fprintf(stderr, "engine: %lu pictures, %lu draws refused\n",
            (unsigned long)b.pictures, (unsigned long)b.refused);
    close_sound(b);
    if (b.gpu) {
        for (Texture &t : b.textures) {
            if (t.live) {
                C3D_TexDelete(&t.tex);
                t.live = false;
            }
        }
        C3D_Fini();
        b.gpu = false;
    }
}
