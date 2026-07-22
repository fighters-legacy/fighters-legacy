// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Streaming HTTP(S) GET HAL (#490). Mirrors IAsyncFilesystem: a single background worker, results
// drained on the main thread via service(). Deliberately minimal — content delivery (a base pack
// download, a mod index fetch) is the only use case. The engine consumes IHttpClient by injection and
// never links a backend; only game/server/tools link the concrete platform-http backend.
//
// TLS certificate validation is ALWAYS on and cannot be disabled; an https->http redirect downgrade
// is always refused. Bodies are streamed in chunks so a multi-gigabyte download never buffers in RAM.

namespace fl {

using HttpRequestId = uint32_t; // 0 = invalid, matching the AsyncReadId convention

enum class HttpStatus : uint8_t {
    Success,   // transfer completed; httpCode is the final HTTP status (e.g. 200, 404)
    Error,     // transport/TLS/setup error before or during transfer; errorMsg is non-null
    Cancelled, // cancel() was called, or a data handler returned false
};

// HTTP method (#143 — the lobby registration client needs POST/DELETE alongside the download GET).
// Delete_ has a trailing underscore to avoid the C++ `delete` keyword.
enum class HttpMethod : uint8_t {
    Get,
    Post,
    Put,
    Delete_,
};

struct HttpRequestOptions {
    std::string url;
    std::string userAgent; // empty -> the backend's default
    HttpMethod method{HttpMethod::Get};
    std::string body;                 // request body for POST/PUT (ignored for GET/DELETE)
    std::string contentType;          // e.g. "application/json"; empty -> none set
    uint32_t connectTimeoutMs{15000}; // 0 -> backend default
    uint32_t totalTimeoutMs{0};       // 0 -> no overall cap
    uint32_t lowSpeedLimitBps{1};     // stall watchdog: abort if slower than this...
    uint32_t lowSpeedTimeS{30};       // ...for this many seconds (0 -> disabled)
    uint32_t maxRedirects{5};         // https->http downgrades are always refused regardless
};

// Implement and register via IHttpClient::setEventHandler. Callbacks fire on the thread that calls
// service() (the main thread in normal use). All pointers are valid only during the call.
class IHttpClientHandler {
  public:
    virtual ~IHttpClientHandler() = default;

    // A chunk of response body. Return false to ABORT the transfer (it then completes as Cancelled).
    // data is valid only during this call — copy or hash it before returning.
    virtual bool onHttpData(HttpRequestId id, const void* data, std::size_t len) = 0;

    // Optional progress. total == 0 when the server did not send a Content-Length.
    virtual void onHttpProgress(HttpRequestId /*id*/, uint64_t /*received*/, uint64_t /*total*/) {}

    // Terminal callback, exactly once per request. httpCode is meaningful only on Success.
    virtual void onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) = 0;
};

class IHttpClient {
  public:
    virtual ~IHttpClient() = default;

    // Starts the background worker. Returns false on failure (see getLastError()).
    virtual bool init() = 0;

    // Cancels all in-flight requests (each fires onHttpComplete with Cancelled), joins the worker.
    virtual void shutdown() = 0;

    // Register the completion handler; nullptr deregisters.
    virtual void setEventHandler(IHttpClientHandler* handler) = 0;

    // Enqueue a request using options.method. Returns an id >= 1, or 0 if it could not be enqueued
    // (before init(), empty url). This is the primitive; get() is a convenience forwarder.
    virtual HttpRequestId request(const HttpRequestOptions& options) = 0;

    // Enqueue a GET (forwards to request() with method = Get). Non-virtual so a backend only implements
    // request(). Kept for the existing download call sites.
    HttpRequestId get(const HttpRequestOptions& options) {
        HttpRequestOptions o = options;
        o.method = HttpMethod::Get;
        return request(o);
    }

    // Best-effort cancellation; the completion callback still fires (Success if already done, else
    // Cancelled). No-op for id 0 or an already-dispatched request.
    virtual void cancel(HttpRequestId id) = 0;

    // Drain completed callbacks. Call once per frame from the main loop.
    virtual void service() = 0;

    // Human-readable last init/shutdown error, or nullptr.
    virtual const char* getLastError() const = 0;
};

} // namespace fl
