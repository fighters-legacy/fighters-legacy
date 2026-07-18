// SPDX-License-Identifier: GPL-3.0-or-later
// ContentDownloader tests (#490): verified download over a canned IHttpClient into a MockFilesystem.
#include <catch2/catch_test_macros.hpp>

#include "content/ContentDownloader.h"
#include "crypto/Sha256.h"

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
