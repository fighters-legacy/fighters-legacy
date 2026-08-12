// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "server_config.h"

#include <ILogger.h>
#include <net/MatchEventLog.h>
#include <net/WorldBroadcaster.h>
#include <replay/ReplayWriter.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ReplayRecorder (#643) — fl-server's owner of the `.flrep` writer.
//
// The recording itself happens on a BACKGROUND THREAD, and that is the whole reason this class
// exists rather than the sink calling ReplayWriter directly. A chunk is zstd-compressed and written
// once per keyframe interval; doing that inline would put a multi-millisecond spike on one sim tick
// every few seconds -- exactly the kind of thing the tick governor exists to notice, caused by a
// feature that has no business affecting the tick at all. So the sim thread's cost is a buffer move
// under a mutex, and everything expensive happens off it.
//
// The queue is bounded by BYTES, and overflow STOPS the recording with a loud log line rather than
// dropping ticks. Ticks cannot be dropped: after a keyframe every record is a delta against the
// previous tick, so a hole in the middle makes everything after it undecodable. A recording that
// stopped and said so is recoverable; one that silently skipped 40 ticks is a corrupt file that
// plays for a while.

namespace fl {

class ReplayRecorder {
  public:
    struct Options {
        ServerConfig::ReplayConfig cfg;
        std::filesystem::path baseDir; // the server's working directory; cfg.dir resolves under it
        std::string baseName;          // file stem (timestamp + mission id)
        std::string engineVersion;
        uint32_t tickRateHz{60};
        double planetRadiusM{0.0};
        std::string missionId;
        uint64_t startUnixSeconds{0};
        uint32_t sessionFlags{0};
    };

    ~ReplayRecorder();

    // Open the file, write header + sections, launch the writer thread. False = not recording (the
    // reason is logged); the server carries on, because a server that refuses to run because it
    // cannot write a replay would be trading the match for the recording of it.
    bool start(const Options& opts, const ReplaySections& sections, ILogger& log);

    // Sim thread. Queues one tick; cheap by construction.
    void onTick(const ReplayTickRecords& rec, std::vector<MatchEvent> events);

    // Flush, close, join. Idempotent; called from the shutdown path.
    void stop();

    [[nodiscard]] bool recording() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }

  private:
    void run(); // writer-thread body

    // 64 MiB of queued payload is minutes of a busy server. Past it, the writer is not keeping up
    // with the disk and the honest move is to stop.
    static constexpr std::size_t kMaxQueuedBytes = 64u * 1024u * 1024u;

    struct Queued {
        ReplayTick tick;
        uint64_t stateHash{0};
        std::size_t bytes{0};
    };

    ReplayWriter m_writer;
    std::filesystem::path m_path;
    std::ofstream m_hashLog;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Queued> m_queue;
    std::size_t m_queuedBytes{0};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    ILogger* m_log{nullptr};
};

} // namespace fl
