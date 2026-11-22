/* ETC1, as the PICA200 reads it, made on the host by host/pack.cpp. a block
 * is 4 by 4 texels, split down the middle or across into two halves of 8,
 * each a base color and one of 8 tables of 4 steps from it; each texel names
 * its step. a half's base is 4 bits a channel, or the first half's 5 and the
 * second's a 3-bit difference from it.
 *
 * the encoder tries each split, each way of writing the bases, and each
 * table, from the half's mean color and the colors near it, and keeps the
 * least squared error. that takes a host some minutes for the whole disc,
 * which is why the 3DS reads the results from its RomFS.
 *
 * the GPU takes a block's 64 bits little-endian, where the format's own
 * description writes them big-endian, and an 8 by 8 tile's four blocks in the
 * order top left, top right, bottom left, bottom right of the GPU's rows.
 * ETC1A4 puts 64 bits of alpha, 4 for each texel, ahead of each block */
#ifndef ENGINE_3DS_HOST_ETC1_H
#define ENGINE_3DS_HOST_ETC1_H

#include <stdint.h>

#include <algorithm>

namespace etc1 {

const int TABLES[8][4] = {{2, 8, -2, -8},       {5, 17, -5, -17},     {9, 29, -9, -29},
                          {13, 42, -13, -42},   {18, 60, -18, -60},   {24, 80, -24, -80},
                          {33, 106, -33, -106}, {47, 183, -47, -183}};

inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* a half's best table and steps for the base color, and their error */
struct Half {
    uint32_t error = UINT32_MAX;
    int table = 0;
    uint8_t steps[8] = {};
};

/* texels holds 8 RGB colors */
inline Half fit(const uint8_t (*texels)[3], const int base[3])
{
    Half best;

    for (int t = 0; t < 8; t++) {
        Half h;

        h.table = t;
        h.error = 0;
        for (int i = 0; i < 8 && h.error < best.error; i++) {
            uint32_t least = UINT32_MAX;

            for (int s = 0; s < 4; s++) {
                uint32_t e = 0;

                for (int c = 0; c < 3; c++) {
                    int d = clamp255(base[c] + TABLES[t][s]) - texels[i][c];

                    e += (uint32_t)(d * d);
                }
                if (e < least) {
                    least = e;
                    h.steps[i] = (uint8_t)s;
                }
            }
            h.error += least;
        }
        if (h.error < best.error) {
            best = h;
        }
    }
    return best;
}

inline int expand4(int c)
{
    return c << 4 | c;
}

inline int expand5(int c)
{
    return c << 3 | c >> 2;
}

/* the block of 16 RGBA texels, block[y][x], as its 64 bits */
inline uint64_t encode(const uint8_t block[4][4][4])
{
    uint64_t best_bits = 0;
    uint32_t best_error = UINT32_MAX;

    for (int flip = 0; flip < 2; flip++) {
        uint8_t halves[2][8][3];
        int places[2][8][2];
        int mean[2][3] = {};

        for (int half = 0; half < 2; half++) {
            int n = 0;

            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int in = flip ? y >> 1 : x >> 1;

                    if (in != half) {
                        continue;
                    }
                    for (int c = 0; c < 3; c++) {
                        halves[half][n][c] = block[y][x][c];
                        mean[half][c] += block[y][x][c];
                    }
                    places[half][n][0] = x;
                    places[half][n][1] = y;
                    n++;
                }
            }
            for (int c = 0; c < 3; c++) {
                mean[half][c] = (mean[half][c] + 4) / 8;
            }
        }
        for (int differential = 0; differential < 2; differential++) {
            int bases[2][3], codes[2][3];
            Half fits[2];
            uint32_t error = 0;
            bool fits_delta = true;

            for (int half = 0; half < 2; half++) {
                /* the quantized mean, and its neighbours a step either side,
                 * each channel's best alone */
                int code[3];

                for (int c = 0; c < 3; c++) {
                    code[c] = differential ? (mean[half][c] * 31 + 127) / 255
                                           : (mean[half][c] * 15 + 127) / 255;
                }
                Half kept;
                int kept_code[3] = {code[0], code[1], code[2]};

                for (int dr = -1; dr <= 1; dr++) {
                    for (int dg = -1; dg <= 1; dg++) {
                        for (int db = -1; db <= 1; db++) {
                            int tried[3] = {code[0] + dr, code[1] + dg, code[2] + db};
                            int most = differential ? 31 : 15;
                            int base[3];

                            if (tried[0] < 0 || tried[1] < 0 || tried[2] < 0 || tried[0] > most ||
                                tried[1] > most || tried[2] > most) {
                                continue;
                            }
                            for (int c = 0; c < 3; c++) {
                                base[c] = differential ? expand5(tried[c]) : expand4(tried[c]);
                            }
                            Half h = fit(halves[half], base);

                            if (h.error < kept.error) {
                                kept = h;
                                kept_code[0] = tried[0];
                                kept_code[1] = tried[1];
                                kept_code[2] = tried[2];
                            }
                        }
                    }
                }
                fits[half] = kept;
                for (int c = 0; c < 3; c++) {
                    codes[half][c] = kept_code[c];
                    bases[half][c] = differential ? expand5(kept_code[c]) : expand4(kept_code[c]);
                }
            }
            if (differential) {
                for (int c = 0; c < 3; c++) {
                    int d = codes[1][c] - codes[0][c];

                    fits_delta = fits_delta && d >= -4 && d <= 3;
                }
                if (!fits_delta) {
                    /* the second half's base, moved within reach of the
                     * first's, and fitted again */
                    for (int c = 0; c < 3; c++) {
                        codes[1][c] = std::min(std::max(codes[1][c], codes[0][c] - 4), codes[0][c] + 3);
                        bases[1][c] = expand5(codes[1][c]);
                    }
                    fits[1] = fit(halves[1], bases[1]);
                }
            }
            error = fits[0].error + fits[1].error;
            if (error >= best_error) {
                continue;
            }
            uint64_t bits = 0;

            if (differential) {
                for (int c = 0; c < 3; c++) {
                    int d = codes[1][c] - codes[0][c];

                    bits |= (uint64_t)codes[0][c] << (59 - c * 8);
                    bits |= (uint64_t)(d & 7) << (56 - c * 8);
                }
            } else {
                for (int c = 0; c < 3; c++) {
                    bits |= (uint64_t)codes[0][c] << (60 - c * 8);
                    bits |= (uint64_t)codes[1][c] << (56 - c * 8);
                }
            }
            bits |= (uint64_t)fits[0].table << 37 | (uint64_t)fits[1].table << 34;
            bits |= (uint64_t)differential << 33 | (uint64_t)flip << 32;
            for (int half = 0; half < 2; half++) {
                for (int i = 0; i < 8; i++) {
                    int x = places[half][i][0], y = places[half][i][1];
                    int j = x * 4 + y;
                    int s = fits[half].steps[i];

                    /* a step's two bits: the low one at j, the high at j + 16 */
                    bits |= (uint64_t)(s & 1) << j | (uint64_t)(s >> 1) << (j + 16);
                }
            }
            best_error = error;
            best_bits = bits;
        }
    }
    return best_bits;
}

/* the block's alphas, 4 bits each, as ETC1A4 takes them: texel (x, y)'s at
 * bit (x * 4 + y) * 4 */
inline uint64_t alphas(const uint8_t block[4][4][4])
{
    uint64_t bits = 0;

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            bits |= (uint64_t)(block[y][x][3] >> 4) << ((x * 4 + y) * 4);
        }
    }
    return bits;
}

/* the decoded block, as the GPU will show it, for checking the encoder */
inline void decode(uint64_t bits, uint8_t out[4][4][3])
{
    bool differential = bits >> 33 & 1, flip = bits >> 32 & 1;
    int bases[2][3];

    for (int c = 0; c < 3; c++) {
        if (differential) {
            int first = (int)(bits >> (59 - c * 8) & 31);
            int d = (int)(bits >> (56 - c * 8) & 7);

            d = d >= 4 ? d - 8 : d;
            bases[0][c] = expand5(first);
            bases[1][c] = expand5(first + d);
        } else {
            bases[0][c] = expand4((int)(bits >> (60 - c * 8) & 15));
            bases[1][c] = expand4((int)(bits >> (56 - c * 8) & 15));
        }
    }
    int tables[2] = {(int)(bits >> 37 & 7), (int)(bits >> 34 & 7)};

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int half = flip ? y >> 1 : x >> 1;
            int j = x * 4 + y;
            int s = (int)((bits >> j & 1) | (bits >> (j + 16) & 1) << 1);

            for (int c = 0; c < 3; c++) {
                out[y][x][c] = (uint8_t)clamp255(bases[half][c] + TABLES[tables[half]][s]);
            }
        }
    }
}

} // namespace etc1

#endif
