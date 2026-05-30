/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Plain-C API over stb_vorbis. ogg_impl.c is the only translation unit that
 * includes stb_vorbis.c; everything else calls these thin wrappers.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Fully decoded PCM result. Call ogg_free_decoded() to release samples. */
typedef struct {
    short* samples; /* interleaved int16_t; numSamples * channels elements */
    int numSamples; /* per-channel sample count; 0 on failure               */
    int sampleRate;
    int channels;
} OggDecoded;

/* Opaque streaming handle. */
typedef struct OggStreamImpl OggStreamImpl;

/* --- Full decode (for short SFX clips) --- */
OggDecoded ogg_decode_memory(const unsigned char* data, int len);
void ogg_free_decoded(OggDecoded* d);

/* --- Streaming (for music tracks) --- */
OggStreamImpl* ogg_stream_open(const unsigned char* data, int len);
int ogg_stream_sample_rate(const OggStreamImpl* s);
int ogg_stream_channels(const OggStreamImpl* s);
/* Returns samples decoded (< numSamples at end-of-stream). */
int ogg_stream_read(OggStreamImpl* s, short* buf, int numSamples);
void ogg_stream_seek_start(OggStreamImpl* s);
void ogg_stream_close(OggStreamImpl* s);

#ifdef __cplusplus
} /* extern "C" */
#endif
