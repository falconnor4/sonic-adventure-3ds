/* the 3DS image's index of the disc's textures, made on the host: index ROOT
 * OUT indexes ROOT/SONICADV's PVMs, PVRs and PRS files as SA1's client does
 * as it starts (engine/sa1/client.cpp), and writes the index to OUT, which
 * the 3DS's client reads from its RomFS in place of reading the whole disc */
#include <stdio.h>

#include "pvr.h"

int main(int argc, char **argv)
{
    pc::Disc disc;
    pc::Textures textures;

    if (argc != 3) {
        fprintf(stderr, "usage: index ROOT OUT\n");
        return 2;
    }
    disc.add_root(argv[1]);
    textures.index(disc, "SONICADV");
    if (textures.count() == 0) {
        fprintf(stderr, "index: %s/SONICADV holds no texture\n", argv[1]);
        return 1;
    }
    if (!textures.write_index(argv[2])) {
        fprintf(stderr, "index: can't write %s\n", argv[2]);
        return 1;
    }
    printf("%s: %u textures\n", argv[2], textures.count());
    return 0;
}
