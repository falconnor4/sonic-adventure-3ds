/* the game's threads on the 3DS, as engine/native/threads.c gives them on a
 * host: thread.c's C keeps each thread's ring and turns, and its four SH-4
 * functions, thread_yield, resume, run and prepare, are these. a context
 * keeps a thread's registers on a stack of its own, since a thread's 4 KB in
 * the heap can't hold the engine's frames.
 *
 * newlib has no ucontext, so a switch is a stack pointer: native_switch
 * pushes what the AAPCS keeps across a call, r4 to r11, the link register and
 * d8 to d15, saves the stack pointer, and loads another's. a .3dsx can't map
 * a guard page, so a stack is plain memory from the heap, made once for its
 * slot and kept */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"

#define STACK_SIZE 0x10000U
#define CONTEXTS 64

extern void *thread_current;
void thread_next(void);
void thread_exit(void);

void native_log(const char *format, ...);

static void stop(const char *what)
{
    native_log("sa1: can't %s", what);
    fprintf(stderr, "sa1: can't %s\n", what);
    exit(2);
}

/* native_switch(from, to): r3 pads the push to a multiple of 8 bytes, so a
 * stack keeps the AAPCS's alignment across it */
void native_switch(void **from, void *to);
__asm__(".text\n"
        ".arm\n"
        ".align 2\n"
        ".globl native_switch\n"
        ".type native_switch, %function\n"
        "native_switch:\n"
        "    push {r3-r11, lr}\n"
        "    vpush {d8-d15}\n"
        "    str sp, [r0]\n"
        "    mov sp, r1\n"
        "    vpop {d8-d15}\n"
        "    pop {r3-r11, pc}\n"
        ".size native_switch, . - native_switch\n");

typedef void *saved;

/* the words native_switch's frame holds: d8 to d15, then r3 to r11 and the
 * return */
#define FRAME_WORDS (16 + 10)

/* a stack whose switch returns into entry. entry never returns, so nothing
 * lies above its frame but the alignment's spare */
static void make(saved *self, char *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size - 16) & ~(uintptr_t)7;
    uint32_t *sp = (uint32_t *)top - FRAME_WORDS;

    memset(sp, 0, FRAME_WORDS * sizeof *sp);
    sp[FRAME_WORDS - 1] = (uint32_t)(uintptr_t)entry;
    *self = sp;
}

static void swap(saved *from, saved *to)
{
    native_switch(from, *to);
}

static void jump(saved *to)
{
    void *left;

    native_switch(&left, *to);
}

static struct context {
    void *thread;
    void (*entry)(void *);
    void *arg;
    saved registers;
} contexts[CONTEXTS];
/* each slot's stack, and the restart context's past them */
static char *stacks[CONTEXTS + 1];
static saved restart;

static char *stack_at(size_t slot)
{
    if (!stacks[slot] && !(stacks[slot] = memalign(16, STACK_SIZE))) {
        stop("make a thread's stack");
    }
    return stacks[slot];
}

/* whether the caller's frames lie on slot's stack */
static int stands_on(size_t slot)
{
    char here;

    return stacks[slot] &&
           (uintptr_t)&here - (uintptr_t)stacks[slot] < STACK_SIZE;
}

/* a slot's stack is made as a thread first takes the slot, and kept */
void native_threads_reserve(void)
{
}

static struct context *context_of(void *thread)
{
    struct context *free_slot = NULL;

    for (int i = 0; i < CONTEXTS; i++) {
        if (contexts[i].thread == thread) {
            return &contexts[i];
        }
        if (!contexts[i].thread && !free_slot && !stands_on((size_t)i)) {
            free_slot = &contexts[i];
        }
    }
    if (!free_slot) {
        stop("run more than 64 threads");
    }
    free_slot->thread = thread;
    return free_slot;
}

static void start(void)
{
    struct context *self = context_of(thread_current);

    self->entry(self->arg);
    thread_exit();
}

/* a context that starts its entry from its stack's top. the stack's words
 * go, so the caller runs on another */
static void renew(struct context *self)
{
    size_t slot = (size_t)(self - contexts);

    if (stands_on(slot)) {
        stop("renew a stack under the thread that runs on it");
    }
    make(&self->registers, stack_at(slot), STACK_SIZE, start);
}

void prepare(void *thread, void *entry, void *arg)
{
    struct context *self = context_of(thread);

    self->entry = (void (*)(void *))entry;
    self->arg = arg;
    renew(self);
}

static void restarted(void)
{
    struct context *self = context_of(thread_current);

    renew(self);
    jump(&self->registers);
}

void run(void *entry, void *arg)
{
    struct context *self = context_of(thread_current);

    self->entry = (void (*)(void *))entry;
    self->arg = arg;
    make(&restart, stack_at(CONTEXTS), STACK_SIZE, restarted);
    jump(&restart);
}

void thread_yield(void)
{
    struct context *self = context_of(thread_current);
    struct context *next;

    thread_next();
    next = context_of(thread_current);
    if (next != self) {
        swap(&self->registers, &next->registers);
    }
}

void resume(void)
{
    jump(&context_of(thread_current)->registers);
}

void native_thread_freed(void *memory)
{
    for (int i = 0; i < CONTEXTS; i++) {
        if (contexts[i].thread == memory) {
            contexts[i].thread = NULL;
        }
    }
}

static saved host, game;

static void game_main(void)
{
    sa1_main();
    jump(&host);
}

void native_run_main(void *stack, size_t size)
{
    make(&game, stack, size, game_main);
    swap(&host, &game);
}
