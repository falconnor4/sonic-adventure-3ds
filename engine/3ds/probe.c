/* the 3DS target's probe: the console's own facts, and a citro3d clear ahead
 * of them, on the top screen and in sdmc:/sa3ds/probe.log. it builds without
 * the disc, so it tells a broken toolchain from a broken game, and it starts
 * with the line a run that never reached main leaves out. make 3ds-probe
 * packages it as build/3ds/sa3ds-probe.3dsx */
#include <stdarg.h>
#include <stdio.h>

#include <sys/stat.h>

#include <3ds.h>
#include <citro3d.h>

#define LOG_DIR "sdmc:/sa3ds"
/* the log's name: each of the probe CIAs (make 3ds-probes) writes its own */
#ifndef PROBE_NAME
#define PROBE_NAME "probe"
#endif
#define LOG_PATH LOG_DIR "/" PROBE_NAME ".log"

/* zeros the size of the game's data and RAM, where -DPROBE_BALLAST asks: a
 * probe that big tells a launch the game's size fails from one its exheader
 * or its icon fails */
#ifdef PROBE_BALLAST
static volatile unsigned char ballast[PROBE_BALLAST] __attribute__((used, aligned(0x1000)));
#endif

static FILE *log_file;

/* a fact goes to the screen and to the log, and the log is flushed as it is
 * written, so a run the console takes down keeps what it said */
static void say(const char *format, ...)
{
    char words[256];
    va_list args;

    va_start(args, format);
    vsnprintf(words, sizeof words, format, args);
    va_end(args);
    printf("%s\n", words);
    if (log_file != NULL) {
        fprintf(log_file, "%s\n", words);
        fflush(log_file);
    }
}

/* citro3d's own start: a command buffer, a target on the top screen and one
 * clear. a backend that cannot do this draws nothing, so the probe tries it
 * before it prints the console's facts */
static int clear_top(void)
{
    C3D_RenderTarget *top;

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return 0;
    }
    top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (top == NULL) {
        C3D_Fini();
        return 0;
    }
    C3D_FrameBegin(0);
    C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0xFF2E3440, 0);
    C3D_FrameDrawOn(top);
    C3D_FrameEnd(0);
    C3D_RenderTargetDelete(top);
    C3D_Fini();
    return 1;
}

/* a region's free bytes, and no wrapped count: an emulator can report more
 * used than the region holds, and the difference would read as 4 GB */
static unsigned long free_bytes(MemRegion region)
{
    u32 size = osGetMemRegionSize(region);
    u32 used = osGetMemRegionUsed(region);

    return used >= size ? 0 : (unsigned long)(size - used);
}

int main(int argc, char **argv)
{
    u16 width = 0;
    u16 height = 0;
    u32 mem_type;

    (void)argc;
    (void)argv;
    (void)mkdir(LOG_DIR, 0777);
    log_file = fopen(LOG_PATH, "w");
    say("sa3ds %s start", PROBE_NAME);
#ifdef PROBE_BALLAST
    ballast[0] = 1;
    ballast[sizeof ballast - 1] = 1;
    say("ballast=%lu bytes", (unsigned long)sizeof ballast);
#endif
    gfxInitDefault();
    say("citro3d=%s", clear_top() ? "ok" : "failed");
    consoleInit(GFX_TOP, NULL);
    mem_type = osGetApplicationMemType();
    say("mem_type=%lu%s", (unsigned long)mem_type, mem_type >= 6 ? " (new 3ds)" : " (old 3ds)");
    say("arm11=%lu hz, new 3ds's %lu hz", (unsigned long)SYSCLOCK_ARM11, (unsigned long)SYSCLOCK_ARM11_NEW);
    if (mem_type >= 6) {
        osSetSpeedupEnable(true);
        say("speedup=enabled");
    }
    say("app=%lu bytes, free=%lu", (unsigned long)osGetMemRegionSize(MEMREGION_APPLICATION),
        free_bytes(MEMREGION_APPLICATION));
    say("system=%lu bytes, free=%lu", (unsigned long)osGetMemRegionSize(MEMREGION_SYSTEM),
        free_bytes(MEMREGION_SYSTEM));
    say("base=%lu bytes, free=%lu", (unsigned long)osGetMemRegionSize(MEMREGION_BASE),
        free_bytes(MEMREGION_BASE));
    gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    {
        Handle limits = 0;
        s64 most = 0, used = 0;
        ResourceLimitType kind = RESLIMIT_COMMIT;

        if (R_SUCCEEDED(svcGetResourceLimit(&limits, CUR_PROCESS_HANDLE))) {
            svcGetResourceLimitLimitValues(&most, limits, &kind, 1);
            svcGetResourceLimitCurrentValues(&used, limits, &kind, 1);
            svcCloseHandle(limits);
        }
        say("commit limit=%lu bytes, used=%lu", (unsigned long)most, (unsigned long)used);
    }
    say("framebuffer=%ux%u", (unsigned)width, (unsigned)height);
    say("probe=ok");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    if (log_file != NULL) {
        fclose(log_file);
    }
    gfxExit();
    return 0;
}
