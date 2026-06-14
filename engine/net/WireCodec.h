// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed, alignment-safe read/write helpers for the packed wire structs in GameProtocol.h.
//
// All GameProtocol structs are #pragma pack(1) PODs; the only correct way to read them out of a raw
// network buffer is std::memcpy (a direct pointer cast of unaligned data is UB and SIGBUSes on
// ARM64). These helpers centralise that rule so server (WorldBroadcaster) and client
// (ClientNetEventHandler) parse/produce packets through one audited path instead of duplicating
// size-check + memcpy boilerplate.
//
// readMsg / readRecordAt : bounds-checked memcpy reads (the portable default).
// viewMsg                : zero-copy const T* when buffer size AND alignment permit, else nullptr —
//                          sound only because the wire layout is naturally aligned (size-multiple
//                          records); callers fall back to readMsg when it returns nullptr.
// appendMsg / writeMsgAt : serialisation helpers so packet builders stop hand-managing offsets.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace fl {

// Read a fixed-size wire struct from the front of a buffer. Returns false (out untouched) if the
// buffer is shorter than sizeof(T).
template <class T> [[nodiscard]] bool readMsg(const void* data, std::size_t size, T& out) {
    static_assert(std::is_trivially_copyable_v<T>, "wire type must be trivially copyable");
    if (size < sizeof(T))
        return false;
    std::memcpy(&out, data, sizeof(T));
    return true;
}

// Read a fixed-size record at a byte offset within a buffer (for variable-length message tails:
// MsgEntityTypeDef[] after MsgConnectAck, MsgEntityEntry[] after MsgWorldSnapshotHeader).
// Returns false if the record would read past the end of the buffer.
template <class T> [[nodiscard]] bool readRecordAt(const void* data, std::size_t size, std::size_t offset, T& out) {
    static_assert(std::is_trivially_copyable_v<T>, "wire type must be trivially copyable");
    if (offset > size || size - offset < sizeof(T))
        return false;
    std::memcpy(&out, static_cast<const std::uint8_t*>(data) + offset, sizeof(T));
    return true;
}

// Zero-copy view into the buffer. Returns a const T* only when the buffer is large enough AND the
// pointer satisfies alignof(T); otherwise nullptr so the caller falls back to readMsg. Safe because
// the wire structs are naturally aligned and ENet/std::vector buffer bases are >= max_align_t.
template <class T> [[nodiscard]] const T* viewMsg(const void* data, std::size_t size) {
    static_assert(std::is_trivially_copyable_v<T>, "wire type must be trivially copyable");
    if (size < sizeof(T))
        return nullptr;
    if (reinterpret_cast<std::uintptr_t>(data) % alignof(T) != 0)
        return nullptr;
    return static_cast<const T*>(data);
}

// Append a fixed-size wire struct (header or record) to a byte buffer.
template <class T> void appendMsg(std::vector<std::uint8_t>& buf, const T& msg) {
    static_assert(std::is_trivially_copyable_v<T>, "wire type must be trivially copyable");
    const auto* p = reinterpret_cast<const std::uint8_t*>(&msg);
    buf.insert(buf.end(), p, p + sizeof(T));
}

// Overwrite a previously-reserved header slot at a byte offset (the snapshot/ack builders append a
// placeholder header, fill the records, then patch the final count back in). Caller guarantees the
// buffer already holds sizeof(T) bytes at offset.
template <class T> void writeMsgAt(std::vector<std::uint8_t>& buf, std::size_t offset, const T& msg) {
    static_assert(std::is_trivially_copyable_v<T>, "wire type must be trivially copyable");
    std::memcpy(buf.data() + offset, &msg, sizeof(T));
}

} // namespace fl
