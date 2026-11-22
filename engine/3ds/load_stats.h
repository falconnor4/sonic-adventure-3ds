/* where a load's time goes on the 3DS: the disc layer (engine/native/gdfs.c),
 * the PRS decoder and the programs' data (engine/native/shim.c,
 * engine/3ds/loads.c) and the client's textures (engine/sa1/client.cpp) add
 * their ticks here, and front.c logs them where a load ends, the first vsync
 * after a second without one, and clears them. the textures load on the
 * drawing's thread where it runs, so their time may overlap the rest */
#ifndef ENGINE_3DS_LOAD_STATS_H
#define ENGINE_3DS_LOAD_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sa3ds_load {
    uint64_t find, open, read, prs, program, textures;
    uint64_t bytes, program_bytes;
    uint32_t opens, reads, decoded, textures_made;
};

extern struct sa3ds_load sa3ds_load;
/* the system's tick, SYSCLOCK_ARM11 a second */
uint64_t sa3ds_tick(void);

#ifdef __cplusplus
}
#endif

#endif
