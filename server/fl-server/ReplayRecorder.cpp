// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReplayRecorder.h"

#include <cstdio>
#include <utility>

namespace fl {

ReplayRecorder::~ReplayRecorder() {
    stop();
}

bool ReplayRecorder::start(const Options& opts, const ReplaySections& sections, ILogger& log) {
    stop();
    m_log = &log;

    ReplayHeader header;
    header.engineVersion = opts.engineVersion;
    header.tickRateHz = opts.tickRateHz;
    header.planetRadiusM = opts.planetRadiusM;
    header.startUnixSeconds = opts.startUnixSeconds;
    header.missionId = opts.missionId;
    header.sessionFlags = opts.sessionFlags;
    header.keyframeIntervalTicks = opts.cfg.keyframeIntervalTicks;

    ReplayWriter::Config wcfg;
    wcfg.dir = opts.baseDir / opts.cfg.dir;
    wcfg.baseName = opts.baseName;
    wcfg.maxFileBytes = static_cast<uint64_t>(opts.cfg.maxFileMb) * 1024ull * 1024ull;
    wcfg.maxFiles = opts.cfg.maxFiles;

    if (!m_writer.open(header, sections, wcfg)) {
        log.log(LogLevel::Warn, __FILE__, __LINE__, ("replay: recording disabled -- " + m_writer.lastError()).c_str());
        return false;
    }
    m_path = m_writer.path();

    if (!opts.cfg.hashLog.empty()) {
        m_hashLog.open(opts.baseDir / opts.cfg.hashLog, std::ios::trunc);
        if (!m_hashLog)
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    "replay: cannot open the state-hash log; continuing without it");
    }

    m_stopping.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&ReplayRecorder::run, this);

    char buf[512];
    std::snprintf(buf, sizeof(buf), "replay: recording to %s (keyframe every %u ticks)", m_path.string().c_str(),
                  opts.cfg.keyframeIntervalTicks);
    log.log(LogLevel::Info, __FILE__, __LINE__, buf);
    return true;
}

void ReplayRecorder::onTick(const ReplayTickRecords& rec, std::vector<MatchEvent> events) {
    if (!m_running.load(std::memory_order_acquire))
        return;

    Queued q;
    q.tick.tickIndex = rec.tick;
    q.tick.flags = rec.keyframe ? kReplayTickKeyframe : uint16_t{0};
    q.tick.recordCount = rec.recordCount;
    q.tick.origins = rec.origins;
    q.tick.records = rec.records;
    q.tick.events = std::move(events);
    q.stateHash = rec.stateHash;
    q.bytes = q.tick.records.size() + q.tick.origins.size() * sizeof(double) + q.tick.events.size() * 64u;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_queuedBytes + q.bytes > kMaxQueuedBytes) {
            // Stop rather than drop: every tick after a keyframe is a delta against the one before
            // it, so a hole would make the rest of the file undecodable and the failure would only
            // surface when someone tried to watch it.
            m_running.store(false, std::memory_order_release);
            if (m_log)
                m_log->log(LogLevel::Warn, __FILE__, __LINE__,
                           "replay: writer fell behind the sim; recording stopped (file remains playable)");
        } else {
            m_queuedBytes += q.bytes;
            m_queue.push_back(std::move(q));
        }
    }
    m_cv.notify_one();
}

void ReplayRecorder::run() {
    for (;;) {
        Queued q;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() { return !m_queue.empty() || m_stopping.load(std::memory_order_acquire); });
            if (m_queue.empty()) {
                if (m_stopping.load(std::memory_order_acquire))
                    return;
                continue;
            }
            q = std::move(m_queue.front());
            m_queue.pop_front();
            m_queuedBytes -= q.bytes;
        }

        if (!m_writer.writeTick(q.tick)) {
            if (m_log)
                m_log->log(LogLevel::Warn, __FILE__, __LINE__,
                           ("replay: " + m_writer.lastError() + "; recording stopped").c_str());
            m_running.store(false, std::memory_order_release);
            return;
        }
        if (m_hashLog.is_open()) {
            // tick, hash, record count. The count is there so a mismatch says WHICH kind of drift it
            // is -- a different world hashing differently, or a different number of entities.
            m_hashLog << q.tick.tickIndex << ' ' << q.stateHash << ' ' << q.tick.recordCount << '\n';
        }
    }
}

void ReplayRecorder::stop() {
    if (!m_thread.joinable()) {
        // start() may have failed after opening the file; close it either way.
        if (m_writer.isOpen())
            m_writer.close();
        if (m_hashLog.is_open())
            m_hashLog.close();
        return;
    }

    m_running.store(false, std::memory_order_release);
    m_stopping.store(true, std::memory_order_release);
    m_cv.notify_all();
    m_thread.join();

    m_writer.close();
    if (m_hashLog.is_open())
        m_hashLog.close();

    if (m_log) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "replay: closed %s (%llu ticks, %llu bytes)", m_path.string().c_str(),
                      static_cast<unsigned long long>(m_writer.ticksWritten()),
                      static_cast<unsigned long long>(m_writer.bytesWritten()));
        m_log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
}

} // namespace fl
