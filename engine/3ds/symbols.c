/* the image's own symbols, on the 3DS: a .3dsx keeps no symbol table, so the
 * build writes the ELF's into the image's RomFS (engine/tools/symtab.py), and
 * this reads it into names.h's list, as the PC's symbols.c reads
 * /proc/self/exe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "names.h"
#include "native.h"

#define SYMBOLS_PATH "romfs:/symbols.bin"

struct entry {
    uint32_t address, size, code, name;
};

/* the table stays in memory, since each name points into it */
static char *table;

static int read_file(const char *path)
{
    FILE *in = fopen(path, "rb");
    long size;
    uint32_t count;
    const struct entry *entries;
    const char *names;

    if (!in) {
        return -1;
    }
    if (fseek(in, 0, SEEK_END) != 0 || (size = ftell(in)) < 8 ||
        fseek(in, 0, SEEK_SET) != 0 || !(table = malloc((size_t)size + 1))) {
        fclose(in);
        return -1;
    }
    if (fread(table, 1, (size_t)size, in) != (size_t)size) {
        fclose(in);
        free(table);
        table = NULL;
        return -1;
    }
    fclose(in);
    table[size] = '\0';
    memcpy(&count, table + 4, 4);
    if (memcmp(table, "SA3S", 4) != 0 ||
        8 + (uint64_t)count * sizeof *entries > (uint64_t)size) {
        return 1;
    }
    entries = (const struct entry *)(table + 8);
    names = table + 8 + count * sizeof *entries;
    for (uint32_t i = 0; i < count; i++) {
        if (names + entries[i].name >= table + size) {
            continue;
        }
        names_symbol_add(entries[i].address, entries[i].size,
                         (int)entries[i].code, names + entries[i].name);
    }
    names_symbols_ready();
    return 0;
}

int native_read_symbols(void)
{
    static int done;
    int failed = done ? 0 : read_file(SYMBOLS_PATH);

    done = !failed;
    return failed;
}

uint32_t native_name_id(void *function)
{
    return names_name_id((uint32_t)(uintptr_t)function);
}

char *native_symbol(const char *name)
{
    native_read_symbols();
    return native_widen(names_symbol(name));
}
