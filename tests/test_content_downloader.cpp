// SPDX-License-Identifier: GPL-3.0-or-later
// ContentDownloader tests (#490): verified download over a canned IHttpClient into a MockFilesystem.
#include <catch2/catch_test_macros.hpp>

#include "content/ContentDownloader.h"
#include "crypto/Sha256.h"
#include "net/LobbyListClient.h"

#include "mock_hal.h"
#include "mock_http.h"

#include <string>

using namespace fl;

namespace {

std::string bodyOf(const MockFilesystem& fs, const std::string& path) {
    auto it = fs.files.find(path);
    if (it == fs.files.end())
        return "<absent>";
    return std::string(it->second.begin(), it->second.end());
}

} // namespace

TEST_CASE("ContentDownloader verifies and installs a good file", "[content][download]") {
    const std::string body = "the base content pack payload, streamed in pieces";
    const std::string hex = sha256Hex(body.data(), body.size());

    TrackingHttpClient http;
    http.setResponse("https://example/pack.dat", body, 200, /*chunkSize=*/8); // force multiple chunks
    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);

    bool done = false, allOk = false;
    dl.start({{"https://example/pack.dat", hex, "pack.dat", body.size()}}, [&](bool ok) {
        done = true;
        allOk = ok;
    });
    http.service();

    CHECK(done);
    CHECK(allOk);
    REQUIRE(dl.results().size() == 1);
    CHECK(dl.results()[0].ok);
    CHECK(bodyOf(fs, "pack.dat") == body);       // installed at the destination
    CHECK(fs.files.count("pack.dat.part") == 0); // temp renamed away
    CHECK(http.requestedUrls.size() == 1);
}

TEST_CASE("ContentDownloader rejects a hash mismatch and installs nothing", "[content][download]") {
    const std::string body = "corrupted-or-tampered bytes";
    const std::string wrongHex = std::string(64, '0'); // definitely not the real digest

    TrackingHttpClient http;
    http.setResponse("https://example/bad.dat", body, 200);
    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);

    bool allOk = true;
    dl.start({{"https://example/bad.dat", wrongHex, "bad.dat", 0}}, [&](bool ok) { allOk = ok; });
    http.service();

    CHECK_FALSE(allOk);
    REQUIRE(dl.results().size() == 1);
    CHECK_FALSE(dl.results()[0].ok);
    CHECK(dl.results()[0].error == "sha256 mismatch");
    CHECK(fs.files.count("bad.dat") == 0);     // never installed
    CHECK(bodyOf(fs, "bad.dat.part").empty()); // partial truncated to zero
    CHECK(log.hasMessage(LogLevel::Error, "mismatch"));
}

TEST_CASE("ContentDownloader reports an HTTP error", "[content][download]") {
    TrackingHttpClient http;
    http.setResponse("https://example/missing.dat", "Not Found", 404);
    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);

    bool allOk = true;
    dl.start({{"https://example/missing.dat", std::string(64, 'a'), "missing.dat", 0}}, [&](bool ok) { allOk = ok; });
    http.service();

    CHECK_FALSE(allOk);
    CHECK(dl.results()[0].error == "HTTP 404");
    CHECK(fs.files.count("missing.dat") == 0);
}

TEST_CASE("ContentDownloader processes multiple entries and fails as a whole on any bad one", "[content][download]") {
    const std::string a = "first file", b = "second file";
    TrackingHttpClient http;
    http.setResponse("https://example/a", a, 200);
    http.setResponse("https://example/b", b, 200);
    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);

    bool allOk = true;
    dl.start(
        {
            {"https://example/a", sha256Hex(a.data(), a.size()), "a.dat", 0},
            {"https://example/b", std::string(64, 'f'), "b.dat", 0}, // wrong hash
        },
        [&](bool ok) { allOk = ok; });
    // Sequential: each entry completes on its own service() pass.
    http.service();
    http.service();

    CHECK_FALSE(allOk);
    REQUIRE(dl.results().size() == 2);
    CHECK(dl.results()[0].ok);       // first installed
    CHECK_FALSE(dl.results()[1].ok); // second rejected
    CHECK(bodyOf(fs, "a.dat") == a);
    CHECK(fs.files.count("b.dat") == 0);
}

TEST_CASE("ContentDownloader with an empty manifest completes ok", "[content][download]") {
    NullHttpClient http;
    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);
    bool done = false, allOk = false;
    dl.start({}, [&](bool ok) {
        done = true;
        allOk = ok;
    });
    CHECK(done);
    CHECK(allOk);
    CHECK_FALSE(dl.inProgress());
}

// ---------------------------------------------------------------------------
// #1083 — the regression that motivated per-request handler routing
// ---------------------------------------------------------------------------
//
// `IHttpClient::setEventHandler` was a SINGLE SLOT, and three components implement the handler
// interface: ContentDownloader, LobbyListClient and LobbyRegistration. ContentDownloader::start took the
// slot and finish() set it back to nullptr rather than restoring whatever was there -- while the client
// already owned that slot for the server browser's lobby list.
//
// So the moment the join-time content download (#924) or the first-run pack download (#108) was wired in,
// the server browser would have silently stopped receiving lobby callbacks after the first download
// completed: no error, no log, just a browser that never populates again. Nothing was broken only because
// the second consumer had not shipped yet. This is that scenario, and it is a precondition for #924 and
// #108 not breaking the browser in M5.
TEST_CASE("two concurrent IHttpClient consumers each get their own completions (#1083)", "[content][download][hal]") {
    const std::string body = "pack bytes";
    const std::string hex = sha256Hex(body.data(), body.size());

    TrackingHttpClient http;
    http.setResponse("https://example/pack.dat", body, 200);
    http.setResponse("https://lobby.example/v1/servers", R"([{"host":"1.2.3.4","port":4778,"name":"Srv"}])", 200);

    MockFilesystem fs;
    MockLogger log;
    ContentDownloader dl(http, fs, log);
    LobbyListClient lobby(http, log);

    // A download runs to completion FIRST -- the step that used to null the shared slot.
    bool downloadDone = false;
    dl.start({{"https://example/pack.dat", hex, "pack.dat", body.size()}}, [&](bool) { downloadDone = true; });
    http.service();
    REQUIRE(downloadDone);
    CHECK(bodyOf(fs, "pack.dat") == body);

    // ...and the server browser still receives its own completion afterwards. Before #1083 the lobby
    // client's handler had been replaced by the downloader's and then cleared, so this refresh completed
    // into nothing and the browser stayed empty for the rest of the process.
    lobby.refresh("https://lobby.example");
    http.service();

    REQUIRE(lobby.servers().size() == 1);
    CHECK(lobby.servers()[0].host == "1.2.3.4");
    CHECK(lobby.servers()[0].port == 4778);

    // And the reverse order: a lobby query in flight is not disturbed by a download completing.
    lobby.refresh("https://lobby.example");
    bool secondDownload = false;
    dl.start({{"https://example/pack.dat", hex, "pack2.dat", body.size()}}, [&](bool) { secondDownload = true; });
    http.service();
    CHECK(secondDownload);
    CHECK(lobby.servers().size() == 1); // the lobby result arrived at the lobby client, not the downloader
    CHECK(bodyOf(fs, "pack2.dat") == body);
}

TEST_CASE("a departing consumer forgets its own requests and nothing else (#1083)", "[content][download][hal]") {
    // cancelRequestsFor is what replaces setEventHandler(nullptr) for a consumer being destroyed:
    // cancel() alone still DELIVERS a completion, straight into the object going away.
    TrackingHttpClient http;
    http.setResponse("https://lobby.example/v1/servers", R"([{"host":"1.2.3.4","port":4778}])", 200);
    MockLogger log;

    LobbyListClient keep(http, log);
    {
        LobbyListClient going(http, log);
        going.refresh("https://lobby.example");
        keep.refresh("https://lobby.example");
        http.cancelRequestsFor(&going); // as a destructor would
    }
    http.service(); // must not touch `going`

    CHECK(keep.servers().size() == 1); // the surviving consumer is unaffected
}
