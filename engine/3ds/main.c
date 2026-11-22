/* the 3DS target's runner: the program's main, in place of the PC's
 * engine/native/runner.c. it starts the console's services, finds the game's
 * statics in the image's own symbols, opens the engine through the front end
 * (front.c), and runs the game's own main on a stack of its own.
 *
 * the game reaches its RAM by constant address. on the PC the runner maps the
 * console's 16 MB at 0x0c000000; a .3dsx maps nothing, so the linker script
 * reserves the 16 MB at SA_3DS_RAM, and rambase.py moves the game's addresses
 * there. the disc is the SD card's sa3ds/disc/, its files as on the disc, and
 * the card's image is sa3ds/card.bin. the log, sa3ds/game.log, holds what the
 * PC's runner writes to stderr */
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "native.h"
#include "sa_engine.h"

#define ROOT "sdmc:/sa3ds"
#define LOG_PATH ROOT "/game.log"
#define DISC_ROOT ROOT "/disc"
/* the disc a CIA may carry in its RomFS, which the build puts there */
#define ROMFS_DISC "romfs:/disc"
#define CARD_PATH ROOT "/card.bin"
/* the game's main thread's stack, as the PC's runner gives it 16 MB: the
 * game's frames are the SH-4's, and the engine's run on it too */
#define GAME_STACK_SIZE 0x00100000U
/* the exception handler's stack */
#define FAULT_STACK_SIZE 0x4000U

const char *native_disc = DISC_ROOT;
int native_target_stage = -1, native_target_act;
struct sa_engine *native_engine;
unsigned long native_vsyncs;
static void (*vsync_function)(void);
void (*native_pace)(void);

static FILE *log_file;
extern volatile uint32_t sa_3ds_audio_peak;
/* every this many vsyncs a screenshot of the top screen goes to
 * sa3ds/shots/, where sa3ds/settings.txt gives "shots N". 0 takes none */
static unsigned long shot_every, shot_from;

/* the log on the SD card. each line goes out at once, so a fault loses none */
void native_log(const char *format, ...)
{
    va_list args;

    if (!log_file) {
        return;
    }
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

/* the shim's reports go to stderr, which the runner points at the log */
static void open_log(void)
{
    mkdir(ROOT, 0777);
    /* the shim and the adapter write to stderr, which becomes the log */
    log_file = freopen(LOG_PATH, "w", stderr);
    if (log_file) {
        setvbuf(log_file, NULL, _IOLBF, 0);
    }
}

void native_report(const char *why)
{
    fprintf(stderr, "sa1: %s after %lu vsyncs: ", why, native_vsyncs);
    native_report_state(stderr);
    fflush(stderr);
}

static void end_run(const char *why, int status) __attribute__((noreturn));
void syBtExit(void) __attribute__((noreturn));

/* the run's end: the hooks hear of it, the front end closes, and the program
 * goes back to the homebrew menu */
static void end_run(const char *why, int status)
{
    native_report(why);
    native_hook_run_end();
    if (native_play_close) {
        native_play_close();
    }
    native_log("exit %d", status);
    fflush(NULL);
    romfsExit();
    gfxExit();
    exit(status);
}

extern int loader_error_flag;

void syBtExit(void)
{
    if (loader_error_flag) {
        end_run("the game left at a disc error", 2);
    }
    end_run("the game left", 0);
}

void port_stop(const char *what, const char *name, uint32_t number)
{
    fprintf(stderr, "sa1: the game stopped: %s %s 0x%x\n", what,
            name ? name : "", (unsigned)number);
    end_run("the game stopped", 2);
}

/* the top screen as a 400 by 240 BMP. the framebuffer is RGB8, its bytes
 * blue, green and red, by columns of 240 from the screen's bottom up, as the
 * screen is turned a quarter: so a BMP's rows, from the bottom, are the
 * framebuffer's rows */
static void screenshot(void)
{
    static const uint8_t head[54] = {
        'B', 'M', 0x36, 0x65, 0x04, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
        0x90, 0x01, 0, 0, 0xf0, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0,
        0, 0x65, 0x04, 0, 0x13, 0x0b, 0, 0, 0x13, 0x0b, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0};
    static uint8_t row[400 * 3];
    char path[64];
    u16 w, h;
    const u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    FILE *out;

    if (!fb || w != 240 || h != 400) {
        return;
    }
    mkdir(ROOT "/shots", 0777);
    snprintf(path, sizeof path, ROOT "/shots/%06lu.bmp", native_vsyncs);
    if (!(out = fopen(path, "wb"))) {
        return;
    }
    fwrite(head, 1, sizeof head, out);
    for (int r = 0; r < 240; r++) {
        for (int x = 0; x < 400; x++) {
            memcpy(&row[x * 3], &fb[(x * 240 + r) * 3], 3);
        }
        fwrite(row, 1, sizeof row, out);
    }
    fclose(out);
}

/* frame.c's waits call it: the front end paces it to the console's 59.94 Hz */
void native_vsync(void)
{
    native_vsyncs++;
    if (native_pace) {
        native_pace();
    }
    if (vsync_function) {
        vsync_function();
    }
    if (native_vsyncs % 600 == 0) {
        struct mallinfo m = mallinfo();

        native_report("at ten seconds");
        native_log("sa3ds: heap %lu of %lu bytes in use, %lu bytes of linear "
                   "memory free",
                   (unsigned long)m.uordblks, (unsigned long)envGetHeapSize(),
                   (unsigned long)linearSpaceFree());
        extern uint64_t sa3ds_ticks_give, sa3ds_ticks_render, sa3ds_ticks_wait,
            sa3ds_ticks_all;
        double ms = 1000.0 / SYSCLOCK_ARM11 / 600.0;

        /* where the drawing's thread draws, its time is its own core's, and
         * the game's thread's waits for it count among the waits */
        extern int sa3ds_drawing_thread;
        uint64_t drawn_here = sa3ds_drawing_thread ? 0 : sa3ds_ticks_give;
        uint64_t spent = drawn_here + sa3ds_ticks_wait;

        native_log("sa3ds: a vsync's %.2f ms: the game %.2f, records and scene "
                   "%.2f, the GPU's submission %.2f, waiting %.2f%s",
                   sa3ds_ticks_all * ms,
                   (sa3ds_ticks_all > spent ? sa3ds_ticks_all - spent : 0) * ms,
                   (sa3ds_ticks_give - sa3ds_ticks_render) * ms,
                   sa3ds_ticks_render * ms, sa3ds_ticks_wait * ms,
                   sa3ds_drawing_thread ? " (the drawing on the third core)" : "");
        {
            extern uint64_t sa3ds_ticks_begin;

            native_log("sa3ds: of which C3D_FrameBegin %.2f",
                       sa3ds_ticks_begin * ms);
        {
            extern float sa3ds_cmdbuf_peak(uint32_t *dropped);
            uint32_t dropped = 0;
            float peak = sa3ds_cmdbuf_peak(&dropped);

            extern volatile uint64_t sa3ds_ticks_audio;

            native_log("sa3ds: the GPU's command buffer at most %d%% full, %lu batches dropped",
                       (int)(peak * 100.0F), (unsigned long)dropped);
            native_log("sa3ds: the sound's mixing took %.1f%% of a core",
                       sa3ds_ticks_all ? 100.0 * (double)sa3ds_ticks_audio / (double)sa3ds_ticks_all
                                       : 0.0);
            sa3ds_ticks_audio = 0;
        }
            sa3ds_ticks_begin = 0;
        }
        sa3ds_ticks_give = sa3ds_ticks_render = sa3ds_ticks_wait = 0;
        sa3ds_ticks_all = 0;
        native_log("sa3ds: the sound's loudest sample %lu",
                   (unsigned long)sa_3ds_audio_peak);
        sa_3ds_audio_peak = 0;
    }
    if (shot_every && native_vsyncs >= shot_from && native_vsyncs % shot_every == 0) {
        screenshot();
    }
}

void njSetVSyncFunction(void (*function)(void))
{
    vsync_function = function;
}

/* there is no domain to map: the 16 MB __system_allocateHeaps takes at
 * __game_ram are the game's RAM, and the threads' stacks come from the heap. a
 * map the shim asks for must lie in the game's RAM */
extern char __game_ram[];

void native_map(void *at, size_t size, const char *what)
{
    uintptr_t from = (uintptr_t)at, ram = (uintptr_t)__game_ram;

    if (from < ram || from + size > ram + 0x01000000U) {
        fprintf(stderr, "sa1: can't map %s at %p\n", what, at);
        end_run("stopped", 2);
    }
    memset(at, 0, size);
}

/* the devices' registers, which rambase.py points the game's casts into */
char sa3ds_devices[0x100000] __attribute__((aligned(32)));
/* the store queues' area: 256 KB that a mask of 18 bits keeps a write in,
 * and a page past it for the 32 bytes a write takes past its address */
char sa3ds_store_queue[0x41000] __attribute__((aligned(0x40000)));

/* refs.py --check's narrowing has nothing to narrow on a 32-bit target */
uint32_t native_narrow(void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

/* syCacheICI's function, which rambase.py has the game call plainly: the
 * ARM11 holds no line of the game's code that a program's read makes stale,
 * since no program's code is read from the disc */
void syCacheICI_P2_(void)
{
}

/* Shinobi's interrupt mask, which the SH-4 keeps in SR: a word here, since the
 * game only saves and restores it around a critical section */
static int interrupt_mask;

void _builtin_set_imask(int mask)
{
    interrupt_mask = mask;
}

int _builtin_get_imask(void)
{
    return interrupt_mask;
}

/* a fault: the log names where and what, and the program ends */
static const char *fault_kind(ERRF_ExceptionType type)
{
    switch (type) {
    case ERRF_EXCEPTION_PREFETCH_ABORT:
        return "prefetch abort";
    case ERRF_EXCEPTION_DATA_ABORT:
        return "data abort";
    case ERRF_EXCEPTION_UNDEFINED:
        return "undefined instruction";
    case ERRF_EXCEPTION_VFP:
        return "VFP";
    default:
        return "exception";
    }
}

static void fault(ERRF_ExceptionInfo *info, CpuRegisters *regs)
{
    native_log("sa1: %s at pc %08lx, address %08lx, lr %08lx, sp %08lx",
               fault_kind(info->type), (unsigned long)regs->pc,
               (unsigned long)info->far, (unsigned long)regs->lr,
               (unsigned long)regs->sp);
    for (int i = 0; i < 13; i++) {
        native_log("  r%d %08lx", i, (unsigned long)regs->r[i]);
    }
    native_report("stopped");
    fflush(NULL);
    svcExitProcess();
}

static void catch_faults(void)
{
    static u8 stack[FAULT_STACK_SIZE] __attribute__((aligned(8)));

    threadOnException(fault, stack + sizeof stack,
                      WRITE_DATA_TO_HANDLER_STACK);
}

/* a message on the bottom screen for a run that can't start, until START */
/* set where the console gave the game too little memory to run, with the
 * memory its resource limit grants, and how much of it the image left */
static int short_of_memory;
static u32 granted, granted_free;

static void refuse(const char *words) __attribute__((noreturn));
static void refuse(const char *words)
{
    native_log("sa1: %s", words);
    consoleInit(GFX_BOTTOM, NULL);
    printf("Sonic Adventure 3DS\n\n%s\n\nPress START to leave.\n", words);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    exit(2);
}

static void find_statics(void)
{
    char words[320];
    const char *lacking;
    int unread = native_read_symbols();

    if (unread) {
        refuse("can't read the image's own symbols, romfs:/symbols.bin: "
               "build the .3dsx with make 3ds, which writes them");
    }
    if ((lacking = sa1_statics_find())) {
        snprintf(words, sizeof words,
                 "the image's symbols hold no object named %s", lacking);
        refuse(words);
    }
}

/* the run's start, from sa3ds/settings.txt where it gives one: a line
 * "stage N" and "act N" begin that act as the opening ends */
static void read_settings(void)
{
    FILE *in = fopen(ROOT "/settings.txt", "r");
    char line[128], keys[96];

    if (!in) {
        return;
    }
    while (fgets(line, sizeof line, in)) {
        int n;

        if (sscanf(line, "stage %d", &n) == 1) {
            native_target_stage = n;
        } else if (sscanf(line, "act %d", &n) == 1) {
            native_target_act = n;
        } else if (sscanf(line, "shots %d", &n) == 1 && n > 0) {
            int from = 0;

            shot_every = (unsigned long)n;
            /* "shots N FROM" starts at vsync FROM */
            if (sscanf(line, "shots %*d %d", &from) == 1 && from > 0) {
                shot_from = (unsigned long)from;
            }
        } else if (sscanf(line, "fps %d", &n) == 1) {
            extern int sa3ds_fps;

            sa3ds_fps = n == 60 ? 60 : 30;
        } else if (sscanf(line, "aa %d", &n) == 1) {
            extern int sa3ds_antialias;

            sa3ds_antialias = n != 0;
        } else if (sscanf(line, "thread %d", &n) == 1) {
            extern int sa3ds_drawing_thread;

            sa3ds_drawing_thread = n != 0;
        } else if (sscanf(line, "keys %95s", keys) == 1) {
            /* a script of keys, as the PC's --keys plays one: the pad's
             * record from it in place of the console's buttons */
            native_keys_open(keys);
        }
    }
    fclose(in);
}

int main(int argc, char **argv)
{
    FILE *first;
    void *stack;
    Result romfs;

    (void)argc;
    (void)argv;
    /* the New 3DS's 804 MHz and its L2 cache, where the console has them */
    osSetSpeedupEnable(true);
    gfxInitDefault();
    romfs = romfsInit();
    open_log();
    native_log("sa3ds: start, romfs %08lx, %lu MB of application memory",
               (unsigned long)romfs,
               (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20));
    catch_faults();
    if (short_of_memory) {
        char words[480];

        snprintf(words, sizeof words,
                 "The console gave the game %lu MB of memory, where it needs "
                 "the New 3DS's 124 MB mode.\n\nMemory type %lu, "
                 "application region %lu MB, %lu MB past the image.\n\n"
                 "Turn the console off and on, then start the game from the "
                 "HOME Menu. If this stays, please send a photo of this "
                 "screen.",
                 (unsigned long)(granted >> 20),
                 (unsigned long)*(volatile u32 *)0x1FF80030,
                 (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20),
                 (unsigned long)(granted_free >> 20));
        refuse(words);
    }
    read_settings();
    /* a CIA that carries the disc in its RomFS reads it there; else the
     * disc lies on the SD card */
    native_disc = ROMFS_DISC;
    if (!(first = native_disc_file("1ST_READ.BIN"))) {
        native_disc = DISC_ROOT;
        first = native_disc_file("1ST_READ.BIN");
    }
    if (!first) {
        refuse("no disc at sdmc:/sa3ds/disc: copy the disc's files there, "
               "1ST_READ.BIN and SONICADV/ among them");
    }
    native_log("sa3ds: the disc at %s", native_disc);
    fclose(first);
    native_card_open(CARD_PATH);
    native_threads_reserve();
    find_statics();
    if (native_play_open) {
        native_play_open(native_disc, 0);
    }
    native_fpu_start();
    native_asm_init();
    native_programs_keep();
    stack = memalign(16, GAME_STACK_SIZE);
    if (!stack) {
        refuse("no memory for the game's stack");
    }
    native_log("sa3ds: the game's main");
    native_run_main(stack, GAME_STACK_SIZE);
    native_report("main returned");
    end_run("main returned", 1);
}

/* the disc's sums, which the PC holds a disc to: the 3DS reads its disc from
 * the SD card, where summing a gigabyte would take minutes, so the table is
 * empty and native_disc_differs passes any disc */
const struct native_disc_sum native_disc_sums[1] = {{NULL, NULL}};
const int native_disc_sum_count = 0;

/* libctru's split of the application's memory past the image: it keeps 32
 * MB of linear memory for the GPU by default, where this program's textures,
 * vertices, framebuffers and sound lie, and gives the rest to the heap,
 * which SA1's client reads the disc's texture files into. this is the least
 * linear heap: __system_allocateHeaps gives it all but HEAP_KEPT */
u32 __ctru_linear_heap_size = 12U << 20;

/* the memory the game needs past its image, heap and linear: under it the
 * console has started the game in a smaller mode */
#define HEAP_NEEDED (40U << 20)

/* the heap a stage keeps past the linear heap: the rest of the 124 MB goes
 * to the linear heap, the textures'. 12 MB left Station Square's road and
 * walls without textures, as the linear heap ran out under them, where the
 * heap held 27 MB in use of 61 MB */
#define HEAP_KEPT (48U << 20)

/* libctru's, which this takes the place of: the heap and the linear heap
 * from the application's memory past the image. libctru's stops the program
 * where there is less than the linear heap asks, before main can say why. a
 * New 3DS sometimes starts a title asking for 124 MB in the Old 3DS's 64 MB,
 * which leaves the image some 11 MB. this then takes a small linear heap, and
 * main tells the player to restart the console */
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap;
extern char *fake_heap_start, *fake_heap_end;
void mappableInit(u32 address_min, u32 address_max);

/* the game's RAM: 16 MB at SA_3DS_RAM, 0x0c000000, the Dreamcast's own
 * address. it lies in the heap's region, past the heap, so a fixed
 * allocation takes it, and the pointers the disc's files hold into
 * themselves, as Station Square's messages do, point into it as they stand */
#define GAME_RAM_BYTES 0x01000000U

void __system_allocateHeaps(void)
{
    Handle limits = 0;
    s64 most = 0, used = 0;
    ResourceLimitType kind = RESLIMIT_COMMIT;
    u32 free, at = 0;

    if (R_FAILED(svcGetResourceLimit(&limits, CUR_PROCESS_HANDLE))) {
        svcBreak(USERBREAK_PANIC);
    }
    svcGetResourceLimitLimitValues(&most, limits, &kind, 1);
    svcGetResourceLimitCurrentValues(&used, limits, &kind, 1);
    svcCloseHandle(limits);
    free = (u32)(most - used) & ~0xfffU;
    granted = (u32)most;
    granted_free = free;
    /* the game's RAM first, at its own address in the heap's region: where
     * it can't be had, main tells the player, and nothing reads it */
    if (free >= GAME_RAM_BYTES &&
        R_SUCCEEDED(svcControlMemory(&at, SA_3DS_RAM, 0, GAME_RAM_BYTES, MEMOP_ALLOC,
                                     MEMPERM_READ | MEMPERM_WRITE))) {
        memset((void *)SA_3DS_RAM, 0, GAME_RAM_BYTES);
        free -= GAME_RAM_BYTES;
    } else {
        short_of_memory = 1;
    }
    if (free < __ctru_linear_heap_size + HEAP_NEEDED) {
        short_of_memory = 1;
        __ctru_linear_heap_size = (free / 2) & ~0xfffU;
    } else if (free > __ctru_linear_heap_size + HEAP_KEPT) {
        __ctru_linear_heap_size = (free - HEAP_KEPT) & ~0xfffU;
    }
    __ctru_heap_size = free - __ctru_linear_heap_size;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    if (R_FAILED(svcControlMemory(&at, __ctru_heap, 0, __ctru_heap_size,
                                  MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)) ||
        R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0,
                                  __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  MEMPERM_READ | MEMPERM_WRITE))) {
        svcBreak(USERBREAK_PANIC);
    }
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
