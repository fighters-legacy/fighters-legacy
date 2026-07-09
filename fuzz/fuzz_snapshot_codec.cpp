// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the quantized world-snapshot decode path — the exact byte stream a client receives
// on the unreliable channel. Mirrors ClientNetEventHandler's parse loop: read the 40-byte header,
// then drive fl::decodeRecord over a fl::BitReader for recordCount records. decodeRecord fails
// closed on truncation/malformed bits, so the invariant we check is simply "no OOB read / no UB"
// under ASan+UBSan for any attacker-controlled buffer.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <net/BitStream.h>
#include <net/GameProtocol.h>
#include <net/SnapshotCodec.h>
#include <net/WireCodec.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fl::MsgWorldSnapshotHeader hdr{};
    if (!fl::readMsg(data, size, hdr))
        return 0;

    // The bitstream occupies bitstreamBytes after the header, clamped to what actually arrived.
    const size_t avail = size - sizeof(hdr);
    const size_t bodyBytes = std::min<size_t>(hdr.bitstreamBytes, avail);

    fl::BitReader reader(data + sizeof(hdr), bodyBytes);
    uint32_t prevIdx = 0;
    for (uint32_t i = 0; i < hdr.recordCount; ++i) {
        fl::QuantEntity qe;
        bool genPresent = false;
        if (!fl::decodeRecord(reader, qe, prevIdx, hdr.frameOrigin, genPresent))
            break;
    }

    // Also exercise the trailing TLV block the client scans after the bitstream (peer count/latency/
    // despawn list), which shares the packet buffer with the record stream.
    if (hdr.bitstreamBytes <= avail) {
        const uint8_t* ext = data + sizeof(hdr) + hdr.bitstreamBytes;
        const size_t extSize = avail - hdr.bitstreamBytes;
        uint16_t pc = 0;
        (void)fl::readExtValue<uint16_t>(ext, extSize, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc);
        uint16_t vlen = 0;
        (void)fl::findExt(ext, extSize, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), vlen);
    }
    return 0;
}
