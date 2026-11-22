/* the 3DS backend's own calls and bounds, beside sa_engine.h's and the PC's
 * header, sa_pc.h, whose borrowing rule and client calls the 3DS keeps: SA1's
 * client and adapter build against it, and the backend answers its calls */
#ifndef SA_3DS_H
#define SA_3DS_H

#include "sa_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the engine's stores: a picture's draws and payloads, the live resources,
 * and the events, which hold a retirement for each */
#define SA_3DS_ARENA (3U << 20)
#define SA_3DS_RESOURCES 8192U
#define SA_3DS_EVENTS (SA_3DS_RESOURCES + 64U)

/* nonzero where the backend plays sound, and where it shows 16:9: the top
 * screen's 400 by 240 is wider than 4:3 */
int sa_3ds_sound(sa_engine *engine);
int sa_3ds_wide(sa_engine *engine);
/* the run ends: the backend lets go of the GPU and the sound */
void sa_3ds_close(sa_engine *engine);

#ifdef __cplusplus
}
#endif

#endif
