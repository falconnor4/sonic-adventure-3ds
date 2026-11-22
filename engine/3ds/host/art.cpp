/* the CIA's icon and banner, from the disc, made on the host: art ROOT DIR
 * writes DIR/icon.png, the disc's label, 0GDTEX.PVR, at 48 by 48, and
 * DIR/banner.png, the label on its own colors at 256 by 128, which the HOME
 * Menu shows as the game is chosen, and DIR/banner.wav, the first seconds of
 * the title's theme, SONICADV/TITLE.ADX, which it plays then. bannertool and
 * smdhtool take them (engine/mk/3ds.mk's 3ds-cia). they hold the disc's own
 * pictures and music, so the CIA is the disc's owner's to keep */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "png.h"
#include "pvr.h"

namespace {

/* the banner's sound: the HOME Menu plays at most about three seconds */
constexpr double BANNER_SECONDS = 2.9;

bool read_file(const std::string &path, std::vector<uint8_t> &bytes)
{
    FILE *in = fopen(path.c_str(), "rb");

    if (in == nullptr) {
        return false;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    bytes.resize(size > 0 ? static_cast<size_t>(size) : 0);
    bool ok = fread(bytes.data(), 1, bytes.size(), in) == bytes.size();
    fclose(in);
    return ok;
}

/* image, area-filtered to width by height, into out's rectangle at x, y of an
 * image out_width wide. a texel's alpha weights its color */
void scaled(const pc::Image &image, uint32_t width, uint32_t height,
            std::vector<uint8_t> &out, uint32_t out_width, uint32_t x,
            uint32_t y)
{
    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            uint32_t x0 = dx * image.width / width, x1 = (dx + 1) * image.width / width;
            uint32_t y0 = dy * image.height / height, y1 = (dy + 1) * image.height / height;
            double sum[4] = {};
            uint32_t n = 0;

            for (uint32_t sy = y0; sy < (y1 > y0 ? y1 : y0 + 1); sy++) {
                for (uint32_t sx = x0; sx < (x1 > x0 ? x1 : x0 + 1); sx++) {
                    const uint8_t *p = &image.rgba[(sy * image.width + sx) * 4];
                    double a = p[3] / 255.0;

                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += a;
                    n++;
                }
            }
            uint8_t *q = &out[((y + dy) * out_width + x + dx) * 4];
            double a = sum[3] / n;
            double under = 1.0 - a;

            /* over what out holds there */
            for (int c = 0; c < 3; c++) {
                double over = sum[3] > 0 ? sum[c] / sum[3] : 0;

                q[c] = static_cast<uint8_t>(over * a + q[c] * under + 0.5);
            }
            q[3] = static_cast<uint8_t>(255.0 * a + q[3] * under + 0.5);
        }
    }
}

uint32_t big16(const uint8_t *at)
{
    return static_cast<uint32_t>(at[0]) << 8 | at[1];
}

uint32_t big32(const uint8_t *at)
{
    return big16(at) << 16 | big16(at + 2);
}

/* the first seconds of an ADX file's samples, by channels, as the sound
 * engine decodes them (engine/pc/sound.cpp) */
bool adx_decode(const std::vector<uint8_t> &adx, double seconds,
                std::vector<int16_t> &pcm, uint32_t &channels, uint32_t &rate)
{
    static const int STEP[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};
    const uint32_t BLOCK = 18, CODES = 32;

    if (adx.size() < 0x2c || big16(adx.data()) != 0x8000 || adx[4] != 3 ||
        adx[5] != BLOCK) {
        return false;
    }
    uint32_t offset = big16(adx.data() + 2) + 4;
    channels = adx[7];
    rate = big32(adx.data() + 8);
    uint32_t samples = big32(adx.data() + 12);
    uint32_t cutoff = big16(adx.data() + 16);
    float a = sqrtf(2.0F) - cosf(6.283185307F * static_cast<float>(cutoff) /
                                 static_cast<float>(rate));
    float b = sqrtf(2.0F) - 1.0F;
    float c = (a - sqrtf((a + b) * (a - b))) / b;
    int32_t k0 = static_cast<int32_t>(c * 8192.0F);
    int32_t k1 = static_cast<int32_t>(-(c * c * 4096.0F));
    uint32_t wanted = static_cast<uint32_t>(seconds * rate);
    int32_t history[2][2] = {};

    if (channels < 1 || channels > 2 || rate == 0) {
        return false;
    }
    wanted = wanted < samples ? wanted : samples;
    pcm.assign(static_cast<size_t>(wanted) * channels, 0);
    for (uint32_t block = 0; block * CODES < wanted; block++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            uint32_t at = offset + (block * channels + ch) * BLOCK;

            if (at + BLOCK > adx.size() || (adx[at] & 0x80)) {
                return true;
            }
            int32_t gain = static_cast<int32_t>(big16(&adx[at]) & 0x1fff) + 1;

            for (uint32_t i = 0; i < CODES; i++) {
                int8_t byte = static_cast<int8_t>(adx[at + 2 + i / 2]);
                int32_t code = i & 1 ? STEP[byte & 15] : byte >> 4;
                int32_t x = gain * code +
                            ((k0 * history[ch][0] + k1 * history[ch][1]) >> 12);

                x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
                history[ch][1] = history[ch][0];
                history[ch][0] = x;
                uint32_t s = block * CODES + i;
                if (s < wanted) {
                    pcm[s * channels + ch] = static_cast<int16_t>(x);
                }
            }
        }
    }
    return true;
}

void put16(FILE *out, uint32_t v)
{
    uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    fwrite(b, 1, 2, out);
}

void put32(FILE *out, uint32_t v)
{
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

/* pcm as a WAV of 16-bit samples, its last tenth of a second faded out */
bool wav_write(const std::string &path, std::vector<int16_t> pcm,
               uint32_t channels, uint32_t rate)
{
    FILE *out = fopen(path.c_str(), "wb");
    uint32_t frames = static_cast<uint32_t>(pcm.size() / channels);
    uint32_t fade = rate / 10 < frames ? rate / 10 : frames;
    uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);

    if (out == nullptr) {
        return false;
    }
    for (uint32_t f = frames - fade; f < frames; f++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            int16_t &s = pcm[f * channels + ch];

            s = static_cast<int16_t>(s * static_cast<int32_t>(frames - f) /
                                     static_cast<int32_t>(fade));
        }
    }
    fwrite("RIFF", 1, 4, out);
    put32(out, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, out);
    put32(out, 16);
    put16(out, 1);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * 2);
    put16(out, channels * 2);
    put16(out, 16);
    fwrite("data", 1, 4, out);
    put32(out, bytes);
    for (int16_t s : pcm) {
        put16(out, static_cast<uint16_t>(s));
    }
    return fclose(out) == 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: art ROOT DIR\n");
        return 2;
    }
    std::string root = argv[1], dir = argv[2];
    std::vector<uint8_t> bytes;
    pc::Image label;
    std::string why;

    if (!read_file(root + "/0GDTEX.PVR", bytes) || !pc::pvr_decode(bytes, 0, label, why)) {
        uint32_t at = 0;

        /* a GBIX chunk before the PVRT */
        if (bytes.size() >= 8 && memcmp(bytes.data(), "GBIX", 4) == 0) {
            at = 8 + (bytes[4] | bytes[5] << 8 | bytes[6] << 16 | bytes[7] << 24);
        }
        if (bytes.empty() || !pc::pvr_decode(bytes, at, label, why)) {
            fprintf(stderr, "art: can't read %s/0GDTEX.PVR: %s\n", root.c_str(), why.c_str());
            return 1;
        }
    }
    /* the icon: the label whole, on black where it is clear */
    std::vector<uint8_t> icon(48 * 48 * 4, 0);

    for (size_t i = 3; i < icon.size(); i += 4) {
        icon[i] = 255;
    }
    scaled(label, 48, 48, icon, 48, 0, 0);
    /* the banner: the label at 128 by 128 in the middle, on its own top
     * left texel's color */
    std::vector<uint8_t> banner(256 * 128 * 4);

    for (size_t i = 0; i < banner.size(); i += 4) {
        memcpy(&banner[i], label.rgba.data(), 3);
        banner[i + 3] = 255;
    }
    scaled(label, 128, 128, banner, 256, 64, 0);
    if (!pc::png_write(dir + "/icon.png", 48, 48, icon) ||
        !pc::png_write(dir + "/banner.png", 256, 128, banner)) {
        fprintf(stderr, "art: can't write the pictures in %s\n", dir.c_str());
        return 1;
    }
    std::vector<int16_t> pcm;
    uint32_t channels = 0, rate = 0;

    if (!read_file(root + "/SONICADV/TITLE.ADX", bytes) ||
        !adx_decode(bytes, BANNER_SECONDS, pcm, channels, rate) ||
        !wav_write(dir + "/banner.wav", pcm, channels, rate)) {
        fprintf(stderr, "art: can't make the banner's sound from %s/SONICADV/TITLE.ADX\n",
                root.c_str());
        return 1;
    }
    printf("%s: icon.png, banner.png and banner.wav, %u Hz, %u channels\n", dir.c_str(), rate,
           channels);
    return 0;
}
