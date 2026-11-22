/* the 3DS target's front end, in place of the PC's engine/sa1/play.c: it
 * makes the engine, hands each picture's records to SA1's client, which turns
 * them into the engine's calls, paces the game's vsyncs at the Dreamcast's
 * 59.94 Hz, and samples the pad as the game reads it. each call into the
 * engine runs under the host's rounding, and gives the game's back.
 *
 * the client, the scene and the GPU's submission run on a thread of their
 * own, on the New 3DS's third core where it has one: a picture's records are
 * copied to it as the game's frame ends, and the game goes on to its next
 * frame while it draws. one picture is in flight: the next frame's end waits
 * for it. the memory card's storage calls, the game's only others that reach
 * the engine's handles and events, take the same lock as the drawing */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "load_stats.h"
#include "native.h"
#include "sa_capture.h"
#include "sa_client.h"
#include "sa_engine.h"
#include "sa_pc.h"
#include "sa_3ds.h"

void native_log(const char *format, ...);
void syBtExit(void) __attribute__((noreturn));

/* the Dreamcast's vsync, at 59.94 Hz, in the ARM11's ticks */
#define VSYNC_TICKS ((uint64_t)SYSCLOCK_ARM11 * 1001ULL / 60000ULL)
/* a run this far behind its clock starts it over rather than race to catch
 * up: six vsyncs */
#define BEHIND_TICKS (VSYNC_TICKS * 6)

static sa_engine *engine;
static sa_pc_client *client;
static uint64_t next_vsync;
static uint32_t pictures, refused;
/* where the ARM11's time goes, in ticks, since the log last read it: the
 * adapter's records through the client and the scene, the GPU's submission
 * within that, and the waits for a vsync. the rest is the game's */
uint64_t sa3ds_ticks_give, sa3ds_ticks_render, sa3ds_ticks_wait, sa3ds_ticks_all;
/* the same drawing and waits, counted from the start and never cleared,
 * which the backend reads a frame at a time to choose 30 or 60 */
uint64_t sa3ds_ticks_give_ever, sa3ds_ticks_wait_ever;
static uint64_t last_vsync_tick;

/* where each thread is, which the watchdog reports: the game's thread at
 * work, waiting for the drawing's thread to take its picture, or waiting for
 * a vsync, and the drawing's thread waiting or drawing */
enum { GAME_WORKING, GAME_WAITING_DRAWING, GAME_WAITING_VSYNC };
static volatile int game_phase, drawing_busy;
static volatile unsigned long drawing_records;
/* the record in hand and the records taken, and the steps of a texture's
 * load (client.cpp, backend.cpp) and the sound's mixing, for the watchdog.
 * records taken that move between two reports say the drawing loops, and
 * still ones that it waits */
static volatile uint32_t record_kind;
static volatile unsigned long records_taken;
volatile int sa3ds_texture_step, sa3ds_audio_busy;

/* the drawing's thread, and the picture handed to it */
LightLock sa3ds_engine_lock;
static Thread drawing;
static LightEvent drawn, due;
static uint8_t *picture;
static size_t picture_bytes, picture_room;
static volatile int drawing_quit, quit_asked;
/* settings.txt's "thread 1" asks for the drawing's thread, on the New 3DS's
 * third core, and "thread 0" refuses it. without either a console draws on
 * the third core, and Azahar, which runs that core far slower than the
 * first, on the game's thread. (the drawing's thread once stopped as Station
 * Square loaded: the sound's mixing, above it on the same core, took nearly
 * all its time. the sound now mixes on the system core beside it) */
int sa3ds_drawing_thread = -1;
/* the top screen's 2x1 supersampling (backend.cpp), which settings.txt's
 * "aa 0" turns off */
int sa3ds_antialias = 1;
/* the pictures a second the game may draw in play: 60 splits each of its 30
 * frames into two pictures, its logic still at 30. settings.txt's "fps 30"
 * keeps one picture a frame */
int sa3ds_fps = 60;

/* Azahar's and Citra's own system information, which a console has none of */
static int emulated(void)
{
    s64 citra = 0;

    return R_SUCCEEDED(svcGetSystemInfo(&citra, 0x20000, 0)) && citra != 0;
}

struct peripheral {
    uint32_t id, support, on, off, press, release;
    uint16_t r, l;
    int16_t x1, y1, x2, y2;
};

void native_play_close(void)
{
    if (!engine) {
        return;
    }
    fprintf(stderr, "sa1: %lu pictures, %lu refused\n", (unsigned long)pictures,
            (unsigned long)refused);
    if (drawing) {
        LightEvent_Wait(&drawn);
        drawing_quit = 1;
        LightEvent_Signal(&due);
        threadJoin(drawing, U64_MAX);
        threadFree(drawing);
        drawing = NULL;
    }
    sa_pc_client_report(client);
    sa1_names_report();
    sa_3ds_close(engine);
}

/* draw one picture's records, on the drawing's thread or the game's */
static void draw_picture(const uint8_t *records, size_t bytes)
{
    uint32_t game = native_fpu_host();
    uint64_t began = svcGetSystemTick();

    for (size_t at = 0; at + sizeof(sa_record_head) <= bytes;) {
        sa_record_head head;
        sa_result result;

        memcpy(&head, records + at, sizeof head);
        if (head.size < sizeof head) {
            break;
        }
        record_kind = head.kind;
        records_taken++;
        result = sa_pc_client_record(client, records + at);
        if (result == SA_PC_QUIT) {
            quit_asked = 1;
        }
        if (result < 0) {
            refused++;
        }
        pictures += result == SA_PC_ENDED;
        at += head.size;
    }
    sa3ds_ticks_give += svcGetSystemTick() - began;
    sa3ds_ticks_give_ever += svcGetSystemTick() - began;
    native_fpu_game(game);
}

static void drawing_main(void *unused)
{
    (void)unused;
    native_fpu_start();
    for (;;) {
        LightEvent_Wait(&due);
        if (drawing_quit) {
            break;
        }
        drawing_busy = 1;
        LightLock_Lock(&sa3ds_engine_lock);
        draw_picture(picture, picture_bytes);
        LightLock_Unlock(&sa3ds_engine_lock);
        drawing_busy = 0;
        drawing_records++;
        LightEvent_Signal(&drawn);
    }
}

/* the game's frame has ended: its records go to the drawing's thread once
 * that has drawn the last picture */
static void give_picture(const uint8_t *records, size_t bytes)
{
    if (!drawing) {
        draw_picture(records, bytes);
    } else {
        uint64_t waited = svcGetSystemTick();

        game_phase = GAME_WAITING_DRAWING;
        LightEvent_Wait(&drawn);
        game_phase = GAME_WORKING;
        waited = svcGetSystemTick() - waited;
        sa3ds_ticks_wait += waited;
        sa3ds_ticks_wait_ever += waited;
        if (bytes > picture_room) {
            uint8_t *more = realloc(picture, bytes);

            if (!more) {
                LightEvent_Signal(&drawn);
                return;
            }
            picture = more;
            picture_room = bytes;
        }
        memcpy(picture, records, bytes);
        picture_bytes = bytes;
        LightEvent_Signal(&due);
    }
    if (quit_asked) {
        syBtExit();
    }
}

static void open_drawing(void)
{
    s32 priority = 0x30;

    LightEvent_Init(&drawn, RESET_ONESHOT);
    LightEvent_Init(&due, RESET_ONESHOT);
    LightEvent_Signal(&drawn);
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    drawing = threadCreate(drawing_main, NULL, 256 * 1024, priority, 2, false);
    native_log("sa1: drawing on %s", drawing ? "the third core" : "the game's thread");
}

/* a vsync: the home menu and the power button get their turn, and the game's
 * clock waits for its time */
struct sa3ds_load sa3ds_load;

uint64_t sa3ds_tick(void)
{
    return svcGetSystemTick();
}

/* a load's time, by where it went: a gap of LOAD_GAP or more between vsyncs
 * is a load, and its figures go to the log as the vsyncs come back */
#define LOAD_GAP (SYSCLOCK_ARM11 / 1)

extern int game_mode;

/* the figures since the last report, under what, over seconds, where any
 * file opened or texture loaded, and then none */
void sa3ds_load_report(const char *what, uint64_t span)
{
    const double ms = 1000.0 / SYSCLOCK_ARM11;
    struct sa3ds_load l = sa3ds_load;

    if (l.opens == 0 && l.textures_made == 0 && l.program_bytes == 0) {
        return;
    }
    memset(&sa3ds_load, 0, sizeof sa3ds_load);
    native_log("sa3ds: %s %.2f s, game_mode %d: %lu opens, finding %.0f ms and opening %.0f; "
               "%lu reads of %.1f MB in %.0f ms; %lu PRS decoded in %.0f ms; programs' data "
               "%.1f MB in %.0f ms; %lu textures in %.0f ms",
               what, span * ms / 1000.0, game_mode, (unsigned long)l.opens, l.find * ms, l.open * ms,
               (unsigned long)l.reads, l.bytes / 1048576.0, l.read * ms,
               (unsigned long)l.decoded, l.prs * ms, l.program_bytes / 1048576.0,
               l.program * ms, (unsigned long)l.textures_made, l.textures * ms);
}

static void wait_vsync(void)
{
    uint64_t now = svcGetSystemTick();

    if (last_vsync_tick && now - last_vsync_tick >= LOAD_GAP) {
        sa3ds_load_report("a load of", now - last_vsync_tick);
    }
    if (last_vsync_tick) {
        sa3ds_ticks_all += now - last_vsync_tick;
    }
    last_vsync_tick = now;
    if (!aptMainLoop()) {
        syBtExit();
    }
    next_vsync += VSYNC_TICKS;
    if (now + BEHIND_TICKS < next_vsync || now > next_vsync + BEHIND_TICKS) {
        next_vsync = now;
        return;
    }
    if (now < next_vsync) {
        game_phase = GAME_WAITING_VSYNC;
        svcSleepThread((s64)((next_vsync - now) * 1000000000ULL /
                             SYSCLOCK_ARM11));
        game_phase = GAME_WORKING;
    }
    sa3ds_ticks_wait += svcGetSystemTick() - now;
    sa3ds_ticks_wait_ever += svcGetSystemTick() - now;
}

/* the watchdog: a console that stops in the game, with no fault, freezes
 * whole, HOME and all, and leaves the log nothing. the watchdog wakes twice
 * a second on the third core, above the drawing's thread, and where no
 * vsync has come for WATCH_STALL seconds it logs where each thread was and
 * the disc file last opened, again every WATCH_AGAIN seconds while it lasts */
#define WATCH_STALL 3
#define WATCH_AGAIN 10
static Thread watchdog;

static void watchdog_main(void *unused)
{
    static const char *const phases[] = {"at work", "waiting for the drawing's thread",
                                         "waiting for a vsync"};
    static const char *const texture_steps[] = {"none", "reading textures.gpu", "making it",
                                                "C3D_TexInit", "copying its texels", "C3D_TexFlush"};
    static const char *const steps[] = {"in the records", "ending the scene",
                                        "waiting for the GPU (C3D_FrameBegin)",
                                        "placing the corners", "drawing the top screen",
                                        "drawing the bottom screen", "ending the frame (C3D_FrameEnd)",
                                        "past C3D_FrameEnd"};
    extern volatile int sa3ds_draw_step;
    unsigned long seen = native_vsyncs, stalled = 0;

    (void)unused;
    for (;;) {
        svcSleepThread(500000000LL);
        if (native_vsyncs != seen) {
            seen = native_vsyncs;
            stalled = 0;
            continue;
        }
        stalled++;
        if (stalled == WATCH_STALL * 2 || (stalled > WATCH_STALL * 2 && stalled % (WATCH_AGAIN * 2) == 0)) {
            native_log("sa3ds: no vsync for %lu s at vsync %lu, game_mode %d: the game's thread %s, "
                       "the drawing's thread %s after %lu pictures, the drawing %s, the last file opened %s",
                       stalled / 2, seen, game_mode, phases[game_phase],
                       drawing ? (drawing_busy ? "drawing" : "waiting") : "absent",
                       drawing_records, steps[sa3ds_draw_step & 7], native_last_opened);
            native_log("sa3ds: the record in hand of kind %lu, %lu records taken, a texture's load at %s, "
                       "the sound %s",
                       (unsigned long)record_kind, records_taken,
                       texture_steps[(unsigned)sa3ds_texture_step % 6],
                       sa3ds_audio_busy ? "mixing" : "waiting");
        }
    }
}

static void open_watchdog(void)
{
    s32 priority = 0x30;

    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    watchdog = threadCreate(watchdog_main, NULL, 16 * 1024, priority - 2, 2, true);
    if (watchdog == NULL) {
        watchdog = threadCreate(watchdog_main, NULL, 16 * 1024, priority - 2, 1, true);
    }
}

/* the buttons held at the last read, kept here and not taken from the game's
 * record: the pause menu clears the record's buttons as it takes START, and a
 * press read against that cleared record came again at the next read while
 * START was still down, and opened the menu again */
static uint32_t held_last;

static void read_pad(void *record)
{
    struct peripheral *pad = record;
    uint32_t game = native_fpu_host();
    sa_pad sample;

    if (sa_input_sample(engine, 0, &sample) == SA_OK) {
        pad->press = sample.on & ~held_last;
        pad->release = held_last & ~sample.on;
        held_last = sample.on;
        pad->on = sample.on;
        pad->off = ~sample.on;
        pad->x1 = sample.x1;
        pad->y1 = sample.y1;
        pad->x2 = sample.x2;
        pad->y2 = sample.y2;
        pad->r = sample.r;
        pad->l = sample.l;
    }
    native_fpu_game(game);
}

static int rumble(uint32_t strength, uint32_t ms, uint32_t fade_ms)
{
    (void)strength;
    (void)ms;
    (void)fade_ms;
    return 0;
}

void native_play_open(const char *disc, int recorded)
{
    sa_engine_desc desc = {.abi = SA_ENGINE_ABI_VERSION,
                           .size = sizeof desc,
                           .game = SA_GAME_SA1,
                           .arena = SA_3DS_ARENA,
                           .pictures = 1,
                           .resources = SA_3DS_RESOURCES,
                           .events = SA_3DS_EVENTS};
    const char *roots[2] = {disc, NULL};
    uint32_t bytes = 0;
    void *memory = NULL;
    sa_result result = sa_engine_memory(&desc, &bytes);

    (void)recorded;
    /* the drawing's thread is chosen before the engine, whose sound takes the
     * core the drawing leaves */
    if (sa3ds_drawing_thread < 0) {
        sa3ds_drawing_thread = !emulated();
    }
    if (result == SA_OK) {
        memory = memalign(32, bytes);
    }
    if (result != SA_OK || !memory ||
        (result = sa_engine_create(&desc, memory, bytes, &engine)) != SA_OK) {
        native_log("sa1: can't make the engine: %d, %lu bytes", (int)result,
                   (unsigned long)bytes);
        exit(2);
    }
    native_log("sa1: the engine in %lu bytes", (unsigned long)bytes);
    client = sa_pc_client_create(engine, roots);
    native_engine = engine;
    native_card_engine = engine;
    native_sound_engine = sa_3ds_sound(engine) ? engine : NULL;
    if (sa_3ds_wide(engine)) {
        sa1_wide_on();
    }
    if (sa3ds_fps == 60) {
        sa1_fast_on();
    }
    next_vsync = svcGetSystemTick();
    LightLock_Init(&sa3ds_engine_lock);
    if (sa3ds_drawing_thread) {
        open_drawing();
    }
    open_watchdog();
    native_picture_sink = give_picture;
    native_pace = wait_vsync;
    /* a script of keys gives the pad in place of the buttons */
    if (!native_scripted) {
        native_pad_read = read_pad;
    }
    native_rumble = rumble;
}
