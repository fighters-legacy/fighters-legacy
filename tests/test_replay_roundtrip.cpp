// SPDX-License-Identifier: GPL-3.0-or-later
//
// replay_roundtrip_ci_smoke (#644) — the record<->replay fidelity gate.
//
// This is the half of #644 that needs a real file: run the REAL server headless with recording on,
// then read the `.flrep` back and require the per-tick state hash computed from the DECODED records
// to equal the hash the recorder computed from the records it encoded.
//
// The hash is taken over the QUANTIZED integer domain (ReplayStateHash.h). That choice is what makes
// the comparison meaningful: quantization is lossy, so a record can never equal its input in float
// space, but it must be exactly equal after quantization -- and any drift in the codec, the chunk
// framing, the compression, the ordering or the delta/keyframe bookkeeping breaks that equality.
//
// The hash is deliberately NOT stored in the file. Storing it would let a recorder assert its own
// correctness, which docs/developer/replay-format.md §6 calls out as not being a test.
//
// The other half of #644 -- does the SIM produce the same world twice -- is in test_world_broadcaster
// ([determinism]), in process, because two networked runs are not tick-aligned and a two-process
// comparison would be flaky rather than strict.

#include "net/ReplayStateHash.h"
#include "replay/ReplayReader.h"

#include "Subprocess.h"
#include <StdoutLogger.h>
#include <net/BitStream.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

// Outside the default 4778/4779 range, and distinct from test_server_shutdown's port so the two may
// run concurrently under `ctest -j`.
constexpr const char* kTestPort = "47797";

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / "flrep_roundtrip";
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct RecordedTick {
    uint64_t hash{0};
    uint32_t recordCount{0};
};

// tick -> {hash, count}, as the recorder saw it (the sidecar written by [replay] hash_log).
std::map<uint64_t, RecordedTick> readHashLog(const fs::path& p) {
    std::map<uint64_t, RecordedTick> out;
    std::ifstream in(p);
    uint64_t tick = 0;
    uint64_t hash = 0;
    uint32_t count = 0;
    while (in >> tick >> hash >> count)
        out[tick] = {hash, count};
    return out;
}

} // namespace

TEST_CASE("a recorded session reads back with identical per-tick state hashes", "[replay][determinism]") {
    TempDir tmp;
    StdoutLogger log;

    const fs::path replayDir = tmp.path / "replays";
    const fs::path hashLog = tmp.path / "hashes.txt";

    // A world with entities in it: recording an empty sim would pass this gate while proving nothing.
    // --test-spawn-ai-count is the existing load-test affordance; the AI keeps everything moving, so
    // most records are deltas and the delta path is exercised, not just keyframes.
    const std::vector<std::string> args{kTestPort,
                                        "1",
                                        "--bind",
                                        "127.0.0.1",
                                        "--sim-worker-threads",
                                        "1",
                                        "--transport",
                                        "enet",
                                        "--replay-dir",
                                        replayDir.string(),
                                        "--replay-hash-log",
                                        hashLog.string(),
                                        "--test-spawn-ai-count",
                                        "12"};

    fl::Subprocess server = fl::Subprocess::spawn(FL_SERVER_BIN, args, /*captureStdout=*/true,
                                                  /*captureStdin=*/true, log);
    REQUIRE(server.valid());

    bool listening = false;
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!listening && std::chrono::steady_clock::now() < startDeadline) {
        const auto line = server.readStdoutLine(1000);
        if (line && line->find("listening on") != std::string::npos)
            listening = true;
    }
    REQUIRE(listening);

    // Let it simulate long enough to cross several keyframe boundaries and fill more than one chunk.
    std::this_thread::sleep_for(std::chrono::seconds(6));
    server.writeStdin("quit");

    bool exited = false;
    const auto exitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < exitDeadline) {
        if (!server.isRunning()) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(exited); // a wedged server would leave a half-written recording (see #1038)

    // ---- the recording ----
    fs::path replayFile;
    for (const auto& e : fs::directory_iterator(replayDir)) {
        if (e.path().extension() == ".flrep") {
            replayFile = e.path();
            break;
        }
    }
    REQUIRE_FALSE(replayFile.empty());

    const std::map<uint64_t, RecordedTick> recorded = readHashLog(hashLog);
    REQUIRE(recorded.size() > 120); // ~2 s of ticks at minimum; a near-empty log is a failed run

    ReplayReader reader;
    REQUIRE(reader.open(replayFile));
    CHECK_FALSE(reader.indexRebuilt()); // the server exited cleanly, so the trailer is there
    CHECK(reader.sections().entityTypes.size() > 0);
    CHECK(reader.header().tickRateHz == 60);
    CHECK(reader.header().planetRadiusM > 0.0);

    // Replay the file, rebuilding each tick's world exactly as a player does (a delta carries no
    // typeIndex, factionIndex or generation), and hash what comes out.
    std::unordered_map<uint32_t, QuantEntity> known;
    std::size_t compared = 0;
    std::size_t deltaTicks = 0;
    ReplayTick tick;
    while (reader.readNextTick(tick)) {
        std::vector<QuantEntity> ents;
        ents.reserve(tick.recordCount);
        BitReader r(tick.records.data(), tick.records.size());
        const auto originCount = static_cast<uint32_t>(tick.origins.size() / 3);
        bool sawDelta = false;
        for (uint16_t i = 0; i < tick.recordCount; ++i) {
            QuantEntity qe;
            bool genPresent = false;
            REQUIRE(decodeStandaloneRecord(r, qe, tick.origins.data(), originCount, genPresent));
            if (qe.isFull) {
                known[qe.idx] = qe;
            } else {
                sawDelta = true;
                const auto it = known.find(qe.idx);
                REQUIRE(it != known.end());
                if (!genPresent)
                    qe.gen = it->second.gen;
                qe.typeIndex = it->second.typeIndex;
                qe.factionIndex = it->second.factionIndex;
                it->second = qe;
            }
            ents.push_back(qe);
        }
        if (sawDelta)
            ++deltaTicks;

        const auto expected = recorded.find(tick.tickIndex);
        if (expected == recorded.end())
            continue; // the sidecar and the file can differ by the last queued tick at shutdown
        INFO("tick " << tick.tickIndex);
        // Count first: it separates "the same entities in a different state" from "a different set
        // of entities", which are very different bugs.
        REQUIRE(ents.size() == expected->second.recordCount);
        REQUIRE(hashTickState(tick.tickIndex, ents.data(), ents.size()) == expected->second.hash);
        ++compared;
    }

    // The gate must have actually compared something, over both record kinds.
    CHECK(compared > 120);
    CHECK(deltaTicks > 0);
}
