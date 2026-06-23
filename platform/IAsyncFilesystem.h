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

// Implement this and register with IAsyncFilesystem::setEventHandler.
// Callbacks are invoked from whichever thread calls IAsyncFilesystem::service()
// (always the main thread in normal use).
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

    // Register the handler that receives completion callbacks from service().
    // Pass nullptr to deregister. Changing the handler while requests are
    // in-flight is valid; completions use the handler current at service() time.
    virtual void setEventHandler(IAsyncFilesystemHandler* handler) = 0;

    // Enqueues an async whole-file read of domain/path. Returns an AsyncReadId
    // >= 1 on success, or 0 if the request could not be enqueued (e.g. before
    // init(), after shutdown(), or path == nullptr).
    virtual AsyncReadId readFileAsync(PathDomain domain, const char* path) = 0;

    // Requests best-effort cancellation of a pending read. The callback always
    // fires: with Success if the worker already completed the read, or Cancelled
    // otherwise. Does nothing if id is 0 or already dispatched.
    virtual void cancelRead(AsyncReadId id) = 0;

    // Drains the internal completion queue and invokes onReadComplete for each
    // finished request. Call once per frame from the main game loop.
    virtual void service() = 0;

    // Returns a human-readable description of the last init/shutdown error,
    // or nullptr if none. Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;
};

} // namespace fl
