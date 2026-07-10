// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: AssetValidator::validate — the magic-byte + size gate AssetManager runs on every
// asset a content pack returns before caching it. The first fuzz byte selects the AssetType; the
// next up-to-16 bytes are the header span; the whole input size is the reported total size. The
// validator never throws (returns a {valid,reason} struct); the invariant is no OOB read / no UB.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "content/AssetValidator.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fl::AssetValidator validator;

    fl::AssetType type = fl::AssetType::Mesh;
    std::span<const uint8_t> header;
    if (size > 0) {
        type = static_cast<fl::AssetType>(data[0] % static_cast<uint8_t>(fl::AssetType::Count));
        const size_t headerLen = std::min<size_t>(size - 1, 16);
        header = std::span<const uint8_t>(data + 1, headerLen);
    }
    (void)validator.validate(type, header, size);
    return 0;
}
