// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"
#include <cstddef>
#include <cstdint>

namespace fl {

// Opaque handle for a pending async read. 0 = invalid, matching the AudioBufferId convention.
using AsyncReadId = uint32_t;

enum class AsyncReadStatus : uint8_t {
    Success,   // data[] contains bytesRead valid bytes
    Error,     // OS error; errorMsg is non-null
    Cancelled, // cancelRead() was called; data and bytesRead are undefined
};

// Implement this and pass PER READ to IAsyncFilesystem::readFileAsync (#1083).
// Callbacks are invoked from whichever thread calls IAsyncFilesystem::service()
// (always the main thread in normal use).
//
// It used to be registered once via a single-slot setEventHandler, with TerrainStreamer assuming sole
// ownership -- the same shape as IHttpClient's slot, which a second consumer would have silently stolen.
// Routing by the id readFileAsync already returns is what the async model implies.
// data and errorMsg are valid only for the duration of the callback; copy any
// bytes you need before returning.
class IAsyncFilesystemHandler {
  public:
    virtual ~IAsyncFilesystemHandler() = default;

    // id        — the AsyncReadId returned by readFileAsync that issued this request
    // status    — Success, Error, or Cancelled
    // data      — pointer to the read bytes; valid only during this call
    // bytesRead — number of bytes read; 0 on error or cancellation
    // errorMsg  — human-readable error; non-null only when status == Error;
    //             valid only during this call
    virtual void onReadComplete(AsyncReadId id, AsyncReadStatus status, const void* data, std::size_t bytesRead,
                                const char* errorMsg) = 0;
};

// Async read-only file I/O for terrain streaming.
// Only whole-file reads are async. Existence checks, directory scans, and writes
// remain on the synchronous IFilesystem. Threading: all methods must be called
// from the main thread; the worker thread is an implementation detail.
class IAsyncFilesystem {
  public:
    virtual ~IAsyncFilesystem() = default;

    // Starts the background I/O thread. Returns false on failure.
    // Must not be called while already initialised; call shutdown() first.
    virtual bool init() = 0;

    // Cancels all pending requests (each fires onReadComplete with Cancelled),
    // joins the worker thread, and frees resources. Safe to call even if init()
    // was never called or already failed.
    virtual void shutdown() = 0;

    // Enqueues an async whole-file read of domain/path, routing its completion to `handler`. Returns an
    // AsyncReadId >= 1 on success, or 0 if the request could not be enqueued (e.g. before init(), after
    // shutdown(), path == nullptr, or handler == nullptr).
    //
    // The handler must outlive the read. A consumer that is going away cancels its reads first --
    // cancelRead() still delivers a completion, so the handler is needed until then.
    virtual AsyncReadId readFileAsync(PathDomain domain, const char* path, IAsyncFilesystemHandler* handler) = 0;

    // Requests best-effort cancellation of a pending read. The callback always
    // fires: with Success if the worker already completed the read, or Cancelled
    // otherwise. Does nothing if id is 0 or already dispatched.
    virtual void cancelRead(AsyncReadId id) = 0;

    // Cancel every in-flight read belonging to `handler` and deliver NOTHING for them.
    //
    // This is what a departing consumer needs, and per-request routing is why it has to exist (#1083).
    // The old single-slot setEventHandler(nullptr) served double duty: it deregistered, which is how
    // TerrainStreamer's destructor made sure a late service() hit a null handler rather than a dead
    // `this`. cancelRead() alone cannot replace it -- it still DELIVERS a Cancelled completion, straight
    // into the object being destroyed. So a handler that is going away says so, once.
    virtual void cancelReadsFor(IAsyncFilesystemHandler* handler) = 0;

    // Drains the internal completion queue and invokes onReadComplete for each
    // finished request. Call once per frame from the main game loop.
    virtual void service() = 0;

    // Returns a human-readable description of the last init/shutdown error,
    // or nullptr if none. Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;
};

} // namespace fl
