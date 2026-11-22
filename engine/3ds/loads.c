/* the programs' data on the 3DS. the PC links every program at once, its
 * data in memory from the start, and engine/native/loads.c puts a program's
 * data back as the loader reads its file again. the 3DS holds no room for
 * all of it: the image overlays the programs that share a Dreamcast origin
 * (engine/tools/overlays.py), as the console does, and each program's data as
 * it starts lies in the RomFS, romfs:/programs/NAME.bin. native_program_read
 * reads it into the program's region, and zeros its bss, as the console's
 * read of the file writes its image over the last one there */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "load_stats.h"
#include "native.h"

void native_log(const char *format, ...);

/* the program a region holds, which a second read of it needs not read again
 * where nothing has written over its data: a program's data takes writes, so
 * every read copies anew */
void native_programs_keep(void)
{
}

static void read_data(const struct native_program *p)
{
    char path[64];
    size_t size = (size_t)(p->data_end - p->data);
    size_t at = 0;
    FILE *in;

    snprintf(path, sizeof path, "romfs:/programs/");
    for (const char *c = p->file; *c && *c != '.' && at < 32; c++, at++) {
        path[16 + at] = (char)tolower((unsigned char)*c);
    }
    snprintf(path + 16 + at, sizeof path - 16 - at, ".bin");
    if (!size) {
        return;
    }
    uint64_t began = sa3ds_tick();

    if (!(in = fopen(path, "rb"))) {
        native_log("sa1: no %s for %s", path, p->file);
        return;
    }
    sa3ds_load.program_bytes += size;
    if (fread(p->data, 1, size, in) != size) {
        native_log("sa1: %s holds less than %s's %lu bytes", path, p->file,
                   (unsigned long)size);
    }
    fclose(in);
    sa3ds_load.program += sa3ds_tick() - began;
}

void native_program_read(const char *file)
{
    for (int i = 0; i < native_program_count; i++) {
        const struct native_program *p = &native_programs[i];

        if (!strcasecmp(file, p->file)) {
            read_data(p);
            memset(p->bss, 0, (size_t)(p->bss_end - p->bss));
            return;
        }
    }
}

/* marks.py's constructors call this on a host. a 32-bit target writes no
 * mark, so nothing calls it there, but a source's prototype names it */
void native_address32(void *object, unsigned long size, int mark, void *address)
{
    (void)object;
    (void)size;
    (void)mark;
    (void)address;
}
