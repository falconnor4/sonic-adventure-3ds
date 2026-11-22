/* the 3DS target's prelude, which each of its compiles reads after the native
 * slice's: mathf.h names no INFINITY, and path.c's non-matching side takes one
 * where a path holds no segment, and the ARM11's FPU takes the place the
 * host's SSE takes, so the calls that switch it are here rather than in
 * native.h's __SSE__ side */
#ifndef ENGINE_3DS_PRELUDE_H
#define ENGINE_3DS_PRELUDE_H

#include <stdint.h>

/* the SH-4's registers and the Dreamcast's devices, as zeros that take
 * writes: rambase.py points the game's casts of their addresses here */
extern char sa3ds_devices[];
/* the store queues' area, 256 KB aligned to its size, which rambase.py points
 * the drawers' store queue writes into */
extern char sa3ds_store_queue[];

#ifndef INFINITY
#define INFINITY (__builtin_inff())
#endif

/* the game's C runs with its rounding from main's first statement, and
 * flushes a denormal result to zero as the SH-4's FPSCR.DN does. flushing
 * also keeps the ARM11's VFP off its support code, which takes a trap for
 * each denormal */
#define FPSCR_FLUSH (1U << 24)
/* the ARM11's VFP runs every operation in hardware only in its RunFast
 * mode: flush to zero, the default NaN, and no exception trapped. outside it
 * a NaN operand or a denormal bounces to support code that the 3DS's kernel
 * doesn't give, and the program faults */
#define FPSCR_DEFAULT_NAN (1U << 25)
#define FPSCR_RUNFAST (FPSCR_FLUSH | FPSCR_DEFAULT_NAN)
#define FPSCR_TRAPS (0x9fU << 8)

/* the game rounds toward zero, as the SH-4's FPU does, and the engine's calls
 * run under round-to-nearest. the PC switches SSE's MXCSR; the ARM11 has one
 * FPU, and FPSCR's bits 22 and 23 hold its rounding, so these switch the same
 * bits and hand the game's value back as the PC hands its MXCSR back */
#define FPSCR_ROUNDING (3U << 22)
#define FPSCR_TOWARD_ZERO (3U << 22)
#define FPSCR_NEAREST 0U

static inline uint32_t fpscr_read(void)
{
    uint32_t value;

    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

static inline void fpscr_write(uint32_t value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
}

static inline uint32_t native_fpu_host(void)
{
    uint32_t game = fpscr_read();

    fpscr_write((game & ~(FPSCR_ROUNDING | FPSCR_TRAPS)) | FPSCR_NEAREST |
                FPSCR_RUNFAST);
    return game;
}

static inline void native_fpu_game(uint32_t game)
{
    fpscr_write(game);
}


static inline void native_fpu_start(void)
{
    fpscr_write((fpscr_read() & ~(FPSCR_ROUNDING | FPSCR_TRAPS)) |
                FPSCR_TOWARD_ZERO | FPSCR_RUNFAST);
}

#endif
