/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Single compilation unit for stb_vorbis. Compiled as C (not C++) so that
 * stb_vorbis.c's C99 constructs (VLAs, compound literals) are valid.
 * CMake adds ${stb_SOURCE_DIR} to the include path and suppresses warnings
 * for this file via set_source_files_properties COMPILE_OPTIONS.
 */

#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c" /* NOLINT */

#include "ogg_impl.h"

#include <stdlib.h>

OggDecoded ogg_decode_memory(const unsigned char* data, int len) {
    OggDecoded result;
    result.samples = NULL;
    result.numSamples = 0;
    result.sampleRate = 0;
    result.channels = 0;

    int numSamples = stb_vorbis_decode_memory(data, len, &result.channels, &result.sampleRate, &result.samples);
    if (numSamples < 0) {
        result.samples = NULL;
        result.numSamples = 0;
    } else {
        result.numSamples = numSamples;
    }
    return result;
}

void ogg_free_decoded(OggDecoded* d) {
    if (d && d->samples) {
        free(d->samples);
        d->samples = NULL;
        d->numSamples = 0;
    }
}

struct OggStreamImpl {
    stb_vorbis* vorbis;
    int sampleRate;
    int channels;
};

OggStreamImpl* ogg_stream_open(const unsigned char* data, int len) {
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, len, &error, NULL);
    if (!v)
        return NULL;

    OggStreamImpl* s = (OggStreamImpl*)malloc(sizeof(OggStreamImpl));
    if (!s) {
        stb_vorbis_close(v);
        return NULL;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    s->vorbis = v;
    s->sampleRate = (int)info.sample_rate;
    s->channels = info.channels;
    return s;
}

int ogg_stream_sample_rate(const OggStreamImpl* s) {
    return s ? s->sampleRate : 0;
}
int ogg_stream_channels(const OggStreamImpl* s) {
    return s ? s->channels : 0;
}

int ogg_stream_read(OggStreamImpl* s, short* buf, int numSamples) {
    if (!s || !s->vorbis || numSamples <= 0)
        return 0;
    return stb_vorbis_get_samples_short_interleaved(s->vorbis, s->channels, buf, numSamples * s->channels);
}

void ogg_stream_seek_start(OggStreamImpl* s) {
    if (s && s->vorbis)
        stb_vorbis_seek_start(s->vorbis);
}

void ogg_stream_close(OggStreamImpl* s) {
    if (!s)
        return;
    if (s->vorbis)
        stb_vorbis_close(s->vorbis);
    free(s);
}
