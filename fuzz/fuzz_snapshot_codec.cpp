// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the quantized world-snapshot decode path — the exact byte stream a client receives on
// the unreliable channel. Mirrors ClientNetEventHandler's parse loop (#725): read the 24-byte header,
// read the shared-origin table, then drive fl::decodeStandaloneRecord over a fl::BitReader for
// recordCount records. Decode fails closed on truncation / malformed bits / an out-of-range origin
// index, so the invariant we check is simply "no OOB read / no UB" under ASan+UBSan for any
// attacker-controlled buffer.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <net/BitStream.h>
#include <net/GameProtocol.h>
#include <net/SnapshotCodec.h>
#include <net/WireCodec.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fl::MsgWorldSnapshotHeader hdr{};
    if (!fl::readMsg(data, size, hdr))
        return 0;

    // Body layout: [origin table: originCount x double[3]][record stream][TLV]. Only read the origin
    // table (and thus allocate for it) when the packet actually contains it — a malformed header can
    // claim a huge originCount, so never size the buffer off the claim alone.
    const size_t originBytes = static_cast<size_t>(hdr.originCount) * 3u * sizeof(double);
    const size_t recordOffset = sizeof(hdr) + originBytes;
    std::vector<double> originTable;
    if (recordOffset <= size && hdr.originCount > 0) {
        originTable.resize(static_cast<size_t>(hdr.originCount) * 3u);
        std::memcpy(originTable.data(), data + sizeof(hdr), originBytes);
    }

    if (recordOffset <= size) {
        const size_t recordAvail = size - recordOffset;
        const size_t bodyBytes = std::min<size_t>(hdr.bitstreamBytes, recordAvail);
        fl::BitReader reader(data + recordOffset, bodyBytes);
        for (uint32_t i = 0; i < hdr.recordCount; ++i) {
            fl::QuantEntity qe;
            bool genPresent = false;
            if (!fl::decodeStandaloneRecord(reader, qe, originTable.data(), hdr.originCount, genPresent))
                break;
        }
    }

    // Also exercise the trailing TLV block the client scans after the record stream (peer count /
    // latency / despawn list), which shares the packet buffer with the record stream.
    const size_t extOffset = recordOffset + hdr.bitstreamBytes;
    if (extOffset <= size) {
        const uint8_t* ext = data + extOffset;
        const size_t extSize = size - extOffset;
        uint16_t pc = 0;
        (void)fl::readExtValue<uint16_t>(ext, extSize, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc);
        uint16_t vlen = 0;
        (void)fl::findExt(ext, extSize, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), vlen);
    }
    return 0;
}
