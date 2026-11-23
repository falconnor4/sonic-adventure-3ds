/* the disc's movies split for the 3DS's CIA, on the host: each SFD's MPEG-1
 * video to NAME.M1V, which the game streams as it plays, and its ADX sound to
 * NAME.MVA after a head of "MVA1" and the video's pictures, a little-endian
 * 32-bit count. an SFD runs to 62 MB, past the memory the 3DS has to read one
 * whole, and its sound, which the game plays from memory, lies spread through
 * it. engine/sa1/movie.c reads the two where it finds them.
 *
 *   movies OUT SFD...   writes OUT/NAME.M1V and OUT/NAME.MVA for each */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static int split(const char *out, const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t length = strcspn(base, ".");
    char video_path[4096], sound_path[4096];
    plm_buffer_t *in = plm_buffer_create_with_filename(path);
    plm_demux_t *demux;
    plm_packet_t *packet;
    FILE *video, *sound;
    uint8_t *adx = NULL;
    size_t adx_bytes = 0, adx_room = 0;
    uint32_t pictures = 0;
    uint8_t tail[3] = {0xff, 0xff, 0xff};
    uint8_t head[8] = {'M', 'V', 'A', '1'};

    if (!in) {
        fprintf(stderr, "movies: can't read %s\n", path);
        return 1;
    }
    snprintf(video_path, sizeof video_path, "%s/%.*s.M1V", out, (int)length, base);
    snprintf(sound_path, sizeof sound_path, "%s/%.*s.MVA", out, (int)length, base);
    if (!(video = fopen(video_path, "wb"))) {
        fprintf(stderr, "movies: can't write %s\n", video_path);
        return 1;
    }
    demux = plm_demux_create(in, 1);
    while ((packet = plm_demux_decode(demux))) {
        if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
            /* a picture's start code, 0 0 1 0, begins each picture, and may
             * straddle two packets */
            for (size_t i = 0; i < packet->length; i++) {
                uint8_t b = packet->data[i];

                pictures += tail[0] == 0 && tail[1] == 0 && tail[2] == 1 && b == 0;
                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = b;
            }
            fwrite(packet->data, 1, packet->length, video);
        } else if (packet->type == PLM_DEMUX_PACKET_AUDIO_1) {
            if (adx_bytes + packet->length > adx_room) {
                adx_room = 2 * (adx_bytes + packet->length);
                adx = realloc(adx, adx_room);
            }
            memcpy(adx + adx_bytes, packet->data, packet->length);
            adx_bytes += packet->length;
        }
    }
    plm_demux_destroy(demux);
    if (fclose(video) != 0 || !(sound = fopen(sound_path, "wb"))) {
        fprintf(stderr, "movies: can't write %s\n", sound_path);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        head[4 + i] = (uint8_t)(pictures >> (8 * i));
    }
    fwrite(head, 1, sizeof head, sound);
    fwrite(adx, 1, adx_bytes, sound);
    free(adx);
    if (fclose(sound) != 0) {
        fprintf(stderr, "movies: can't write %s\n", sound_path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int failed = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: movies OUT SFD...\n");
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        failed |= split(argv[1], argv[i]);
    }
    return failed;
}
