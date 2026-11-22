/* the PICA200's textures, as the 3DS's backend makes them while the game runs
 * and as the host's packer (host/pack.cpp) makes them ahead of it: a power of
 * two on each side, from 8 to 1024, the rows from the bottom up, in 8 by 8
 * tiles whose texels lie in Morton order. a 16-bit texture holds RGB565,
 * RGBA5551 or RGBA4; ETC1 holds 4 bits a texel, and ETC1A4 4 bits of alpha
 * besides. the packer's file, romfs:/textures.gpu, holds each texture of
 * the disc's index ready to copy, which SA_FORMAT_GPU hands the backend */
#ifndef ENGINE_3DS_TEXELS_H
#define ENGINE_3DS_TEXELS_H

#include <stdint.h>
#include <string.h>

/* the 3DS backend's own resource format, beside sa_engine.h's: an
 * sa_gpu_texels, then the texels as the GPU reads them */
#define SA_FORMAT_GPU 0x3d50

/* the GPU's formats, as citro3d's GPU_TEXCOLOR numbers them */
enum {
    TEXELS_RGBA5551 = 0x2,
    TEXELS_RGB565 = 0x3,
    TEXELS_RGBA4 = 0x4,
    TEXELS_ETC1 = 0xc,
    TEXELS_ETC1A4 = 0xd
};

typedef struct sa_gpu_texels {
    uint16_t width;
    uint16_t height;
    uint32_t format; /* TEXELS_* */
} sa_gpu_texels;

/* textures.gpu: the magic, the count of the index's places, then for each
 * its offset and its bytes, an sa_gpu_texels and the texels. a place the
 * packer can't decode has no bytes */
#define TEXELS_MAGIC "SA3T"
typedef struct sa_gpu_pack_entry {
    uint32_t offset;
    uint32_t bytes;
} sa_gpu_pack_entry;

#ifdef __cplusplus
/* the bytes of a texture of these sides and format */
inline uint32_t texels_bytes(uint32_t width, uint32_t height, uint32_t format)
{
    uint32_t texels = width * height;

    return format == TEXELS_ETC1 ? texels / 2 : format == TEXELS_ETC1A4 ? texels : texels * 2;
}

/* the side the GPU takes for a side of n: the power of two at or past it,
 * from 8 to 1024 */
inline uint32_t texels_side(uint32_t n)
{
    uint32_t p = 8;

    while (p < n && p < 1024) {
        p <<= 1;
    }
    return p;
}

/* a texel's place in its 8 by 8 tile: x's and y's low three bits, by turns */
struct TexelsMorton {
    uint8_t at[64];

    TexelsMorton()
    {
        for (uint32_t y = 0; y < 8; y++) {
            for (uint32_t x = 0; x < 8; x++) {
                at[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                          ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
            }
        }
    }
};

/* rgba's 16-bit format: none of its alphas below 255 is RGB565, only 0 and
 * 255 RGBA5551, and any other RGBA4 */
inline uint32_t texels_format16(const uint8_t *rgba, uint32_t width, uint32_t height)
{
    bool opaque = true, binary = true;
    size_t count = (size_t)width * height;

    for (size_t i = 0; i < count && (opaque || binary); i++) {
        uint8_t a = rgba[i * 4 + 3];

        opaque = opaque && a == 255;
        binary = binary && (a == 0 || a == 255);
    }
    return opaque ? TEXELS_RGB565 : binary ? TEXELS_RGBA5551 : TEXELS_RGBA4;
}

/* the 16-bit texels of rgba, width by height and top row first, at out's w
 * by h, each from the image's nearest texel. columns holds w words */
inline void texels_make16(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t format,
                          uint16_t *out, uint32_t w, uint32_t h, uint32_t *columns)
{
    static const TexelsMorton morton;

    /* each texel's column in the image, found once: the ARM11 divides in
     * software, and a movie's frames come 30 times a second */
    for (uint32_t x = 0; x < w; x++) {
        columns[x] = width == w ? x : x * width / w;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *line = &rgba[(size_t)(height == h ? y : y * height / h) * width * 4];
        /* the GPU's row y from the bottom holds the image's row from the top
         * that lies h - 1 - y down */
        uint32_t gy = h - 1 - y;
        uint16_t *row = out + (size_t)(gy >> 3) * (w >> 3) * 64;
        const uint8_t *place = &morton.at[(gy & 7) * 8];

        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = &line[columns[x] * 4];
            uint16_t v;

            if (format == TEXELS_RGB565) {
                v = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
            } else if (format == TEXELS_RGBA5551) {
                v = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >> 7));
            } else {
                v = (uint16_t)((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4);
            }
            row[(x >> 3) * 64 + place[x & 7]] = v;
        }
    }
}
#endif

#endif
