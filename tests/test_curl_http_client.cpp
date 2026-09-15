// SPDX-License-Identifier: GPL-3.0-or-later
// The libcurl IHttpClient backend against a real loopback server (#1399). Every other HTTP test in the
// tree drives a mock; this one exists because the defect it pins passed review on a mock-shaped
// reading — "deregister(); service(); shutdown();" LOOKS like it sends the DELETE, and only a real
// worker thread shows that service() drains completions rather than sending anything and shutdown()
// cancels what is still queued. Built only when libcurl is present (tests/CMakeLists.txt); if it is
// built, the backend MUST exist — a nullptr from the factory is a build inconsistency, not a skip.
#include "http/CurlHttpClientFactory.h"

#include "mock_hal.h"

#include <httplib.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>

using namespace fl;

namespace {

// A loopback HTTP server on an OS-picked port (#787: never a fixed one), serving DELETE /v1/servers
// the way fl-lobby does: 204 when the JSON body names a port, 400 "port is required" when it does
// not -- which is exactly what a body-less DELETE drew, and why the entry was never dropped.
struct LoopbackLobby {
    httplib::Server svr;
    std::thread thread;
    int port{0};
    std::atomic<int> deletes{0};  // DELETEs that carried a port and were honoured
    std::atomic<int> rejected{0}; // DELETEs that arrived without one (the #1399 shape)
    std::chrono::milliseconds delay{0};

    explicit LoopbackLobby(std::chrono::milliseconds d = std::chrono::milliseconds(0)) : delay(d) {
        svr.Delete("/v1/servers", [this](const httplib::Request& req, httplib::Response& res) {
            if (delay.count() > 0)
                std::this_thread::sleep_for(delay);
            if (req.body.find("\"port\":4778") == std::string::npos) {
                ++rejected;
                res.status = 400;
                res.set_content("{\"error\":\"port is required\"}", "application/json");
                return;
            }
            ++deletes;
            res.status = 204;
        });
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread([this] { svr.listen_after_bind(); });
        // listen_after_bind() returns immediately if it fails; is_running() is the honest readiness gate.
        for (int i = 0; i < 200 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(svr.is_running());
    }
    ~LoopbackLobby() {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1/servers";
    }
};

struct Recorder : public IHttpClientHandler {
    std::atomic<int> completions{0};
    HttpStatus last{HttpStatus::Error};
    long lastCode{0};
    bool onHttpData(HttpRequestId, const void*, std::size_t) override {
        return true;
    }
    void onHttpComplete(HttpRequestId, HttpStatus status, long httpCode, const char*) override {
        last = status;
        lastCode = httpCode;
        ++completions;
    }
};

HttpRequestOptions deleteOf(const LoopbackLobby& lobby) {
    HttpRequestOptions o;
    o.url = lobby.url();
    o.method = HttpMethod::Delete_;
    o.contentType = "application/json";
    o.body = "{\"port\":4778}";
    return o;
}

} // namespace

TEST_CASE("CurlHttpClient: flush() lets a queued DELETE reach the server before shutdown (#1399)",
          "[http][integration]") {
    NullLogger log;
    LoopbackLobby lobby;
    auto client = createHttpClient(&log);
    REQUIRE(client != nullptr); // built with libcurl: the backend must be there
    REQUIRE(client->init());

    Recorder rec;
    REQUIRE(client->request(deleteOf(lobby), &rec) != 0);

    // The exact shutdown sequence fl-server runs. Before #1399 it was "service(); shutdown();": the
    // DELETE did leave (the worker pops it before the join) but WITHOUT its body, so the lobby answered
    // 400 and kept the entry -- and the 400 was never delivered, because shutdown() drops completions.
    // Both halves are asserted here: the body arrived (204, not 400) and the completion was delivered.
    CHECK(client->flush(std::chrono::seconds(5)));
    CHECK(lobby.deletes.load() == 1);
    CHECK(lobby.rejected.load() == 0);
    CHECK(rec.completions.load() == 1); // delivered by the flush itself
    CHECK(rec.last == HttpStatus::Success);
    CHECK(rec.lastCode == 204);
    client->shutdown();
    CHECK(rec.completions.load() == 1); // nothing left for shutdown to cancel
}

TEST_CASE("CurlHttpClient: flush() gives up on a hung server within its timeout (#1399)", "[http][integration]") {
    NullLogger log;
    LoopbackLobby lobby(std::chrono::milliseconds(1500)); // slower than the flush budget
    auto client = createHttpClient(&log);
    REQUIRE(client != nullptr);
    REQUIRE(client->init());

    Recorder rec;
    REQUIRE(client->request(deleteOf(lobby), &rec) != 0);

    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(client->flush(std::chrono::milliseconds(200)));
    const auto waited = std::chrono::steady_clock::now() - t0;
    // The cap is what keeps a hung lobby from holding fl-server's shutdown hostage. Generous upper
    // bound: a loaded CI box, not a stopwatch.
    CHECK(waited >= std::chrono::milliseconds(200));
    CHECK(waited < std::chrono::milliseconds(1200));
    CHECK(rec.completions.load() == 0); // still in flight
    client->shutdown();                 // joins the worker, which finishes the transfer the server is sitting on
}

TEST_CASE("CurlHttpClient: flush() with nothing pending returns at once (#1399)", "[http][integration]") {
    NullLogger log;
    auto client = createHttpClient(&log);
    REQUIRE(client != nullptr);
    REQUIRE(client->init());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(client->flush(std::chrono::seconds(5)));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    client->shutdown();
}
