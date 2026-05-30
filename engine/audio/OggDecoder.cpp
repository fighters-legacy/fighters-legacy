// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/OggDecoder.h"
#include "audio/ogg_impl.h"

DecodedPcm decodeOgg(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return {};

    OggDecoded raw = ogg_decode_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!raw.samples || raw.numSamples <= 0)
        return {};

    DecodedPcm result;
    result.sampleRate = raw.sampleRate;
    result.channels = raw.channels;
    result.samples.assign(raw.samples, raw.samples + static_cast<std::size_t>(raw.numSamples) * raw.channels);
    ogg_free_decoded(&raw);
    return result;
}

OggStream* openOggStream(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return nullptr;
    return reinterpret_cast<OggStream*>(ogg_stream_open(bytes.data(), static_cast<int>(bytes.size())));
}

OggStreamInfo getOggStreamInfo(const OggStream* stream) {
    const auto* s = reinterpret_cast<const OggStreamImpl*>(stream);
    if (!s)
        return {};
    return {ogg_stream_sample_rate(s), ogg_stream_channels(s)};
}

int readOggSamples(OggStream* stream, int16_t* buf, int numSamples) {
    return ogg_stream_read(reinterpret_cast<OggStreamImpl*>(stream), buf, numSamples);
}

void seekOggStart(OggStream* stream) {
    ogg_stream_seek_start(reinterpret_cast<OggStreamImpl*>(stream));
}

void closeOggStream(OggStream* stream) {
    ogg_stream_close(reinterpret_cast<OggStreamImpl*>(stream));
}
