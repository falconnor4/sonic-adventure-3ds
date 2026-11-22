/* the 3DS image's textures, made on the host: pack ROOT INDEX OUT decodes
 * each place of INDEX, the index host/index.cpp wrote of ROOT's disc, and
 * writes OUT, romfs:/textures.gpu, each texture as the PICA200 reads it
 * (engine/3ds/texels.h). the 3DS's client copies one from there as the game
 * asks for it, in place of reading its PVM, decoding it and converting it.
 *
 * a texture whose sides are both at least 64 goes as ETC1, 4 bits a texel,
 * or ETC1A4 where any alpha is below 255; a smaller one keeps the 16 bits
 * the backend's own conversion gives, which sharp text and the HUD's shapes
 * keep their edges in. the work runs on each of the host's cores */
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "disc.h"
#include "etc1.h"
#include "pvr.h"
#include "texels.h"

namespace {

/* the side at or past which a texture is compressed */
constexpr uint32_t ETC1_SIDE = 64;

struct Made {
    std::vector<uint8_t> bytes; /* sa_gpu_texels and texels, or none */
};

/* the texels of image as the GPU reads them, after their sa_gpu_texels */
std::vector<uint8_t> make(const pc::Image &image)
{
    uint32_t w = texels_side(image.width), h = texels_side(image.height);
    uint32_t format16 = texels_format16(image.rgba.data(), image.width, image.height);
    bool compress = w >= ETC1_SIDE && h >= ETC1_SIDE;
    uint32_t format = !compress ? format16 : format16 == TEXELS_RGB565 ? TEXELS_ETC1 : TEXELS_ETC1A4;
    sa_gpu_texels head{static_cast<uint16_t>(w), static_cast<uint16_t>(h), format};
    std::vector<uint8_t> out(sizeof head + texels_bytes(w, h, format));

    memcpy(out.data(), &head, sizeof head);
    uint8_t *texels = out.data() + sizeof head;

    if (!compress) {
        std::vector<uint32_t> columns(w);

        texels_make16(image.rgba.data(), image.width, image.height, format,
                      reinterpret_cast<uint16_t *>(texels), w, h, columns.data());
        return out;
    }
    /* the image at w by h, nearest texel, its rows from the bottom up as the
     * GPU's run */
    std::vector<uint8_t> rows((size_t)w * h * 4);

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *line = &image.rgba[(size_t)(y * image.height / h) * image.width * 4];
        uint8_t *to = &rows[(size_t)(h - 1 - y) * w * 4];

        for (uint32_t x = 0; x < w; x++) {
            memcpy(&to[x * 4], &line[(x * image.width / w) * 4], 4);
        }
    }
    /* each 8 by 8 tile, its four blocks in turn */
    size_t at = 0;

    for (uint32_t ty = 0; ty < h; ty += 8) {
        for (uint32_t tx = 0; tx < w; tx += 8) {
            static const uint32_t corners[4][2] = {{0, 0}, {4, 0}, {0, 4}, {4, 4}};

            for (const auto &corner : corners) {
                uint8_t block[4][4][4];

                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        memcpy(block[y][x], &rows[((size_t)(ty + corner[1] + y) * w + tx + corner[0] + x) * 4], 4);
                    }
                }
                if (format == TEXELS_ETC1A4) {
                    uint64_t a = etc1::alphas(block);

                    memcpy(texels + at, &a, sizeof a);
                    at += sizeof a;
                }
                uint64_t bits = etc1::encode(block);

                memcpy(texels + at, &bits, sizeof bits);
                at += sizeof bits;
            }
        }
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    pc::Disc disc;
    pc::Textures textures;

    if (argc != 4) {
        fprintf(stderr, "usage: pack ROOT INDEX OUT\n");
        return 2;
    }
    disc.add_root(argv[1]);
    if (!textures.read_index(argv[2])) {
        fprintf(stderr, "pack: can't read %s\n", argv[2]);
        return 1;
    }
    const std::vector<pc::PvrPlace> &places = textures.places();
    std::vector<Made> made(places.size());
    /* each file's places, so a thread decompresses a file once */
    std::map<std::string, std::vector<size_t>> files;

    for (size_t i = 0; i < places.size(); i++) {
        files[places[i].path].push_back(i);
    }
    std::vector<const std::pair<const std::string, std::vector<size_t>> *> work;

    for (const auto &file : files) {
        work.push_back(&file);
    }
    std::atomic<size_t> next{0}, failed{0};
    std::mutex said;
    auto worker = [&]() {
        for (size_t w; (w = next++) < work.size();) {
            const std::string &path = work[w]->first;
            std::vector<uint8_t> bytes, plain;

            if (!disc.read(path, bytes)) {
                failed += work[w]->second.size();
                continue;
            }
            bool compressed = places[work[w]->second.front()].compressed;

            if (compressed && !pc::prs_decode(bytes, plain)) {
                failed += work[w]->second.size();
                continue;
            }
            const std::vector<uint8_t> &file = compressed ? plain : bytes;

            for (size_t i : work[w]->second) {
                pc::Image image;
                std::string why;

                if (!pc::pvr_decode(file, places[i].at, image, why)) {
                    std::lock_guard<std::mutex> lock(said);

                    fprintf(stderr, "pack: %s's %s: %s\n", path.c_str(), places[i].name.c_str(), why.c_str());
                    failed++;
                    continue;
                }
                made[i].bytes = make(image);
            }
        }
    };
    std::vector<std::thread> threads;
    unsigned cores = std::max(1U, std::thread::hardware_concurrency());

    for (unsigned i = 0; i < cores; i++) {
        threads.emplace_back(worker);
    }
    for (std::thread &t : threads) {
        t.join();
    }
    FILE *out = fopen(argv[3], "wb");

    if (!out) {
        fprintf(stderr, "pack: can't write %s\n", argv[3]);
        return 1;
    }
    uint32_t count = static_cast<uint32_t>(places.size());
    std::vector<sa_gpu_pack_entry> entries(count);
    /* the texels start 16-byte aligned, past the table */
    uint32_t at = (8 + count * static_cast<uint32_t>(sizeof(sa_gpu_pack_entry)) + 15) & ~15U;
    size_t etc = 0, sixteen = 0;

    for (uint32_t i = 0; i < count; i++) {
        entries[i].offset = made[i].bytes.empty() ? 0 : at;
        entries[i].bytes = static_cast<uint32_t>(made[i].bytes.size());
        at = (at + entries[i].bytes + 15) & ~15U;
        if (!made[i].bytes.empty()) {
            sa_gpu_texels head;

            memcpy(&head, made[i].bytes.data(), sizeof head);
            (head.format == TEXELS_ETC1 || head.format == TEXELS_ETC1A4 ? etc : sixteen)++;
        }
    }
    fwrite(TEXELS_MAGIC, 1, 4, out);
    fwrite(&count, sizeof count, 1, out);
    fwrite(entries.data(), sizeof(sa_gpu_pack_entry), count, out);
    for (uint32_t i = 0; i < count; i++) {
        if (made[i].bytes.empty()) {
            continue;
        }
        fseek(out, entries[i].offset, SEEK_SET);
        fwrite(made[i].bytes.data(), 1, made[i].bytes.size(), out);
    }
    fseek(out, at, SEEK_SET);
    fclose(out);
    printf("%s: %u textures, %zu ETC1, %zu 16-bit, %zu failed, %u bytes\n", argv[3], count, etc, sixteen,
           failed.load(), at);
    return 0;
}
