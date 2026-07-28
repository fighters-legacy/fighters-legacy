// SPDX-License-Identifier: GPL-3.0-or-later
#include "ISpeechToText.h"
#include "stt/NullSpeechToText.h"

#include <ILogger.h>

#if defined(FL_HAVE_WHISPER)
#include <whisper.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#endif

// whisper.cpp backend for the deterministic voice-command tier (#935).
//
// Built only when FL_ENABLE_WHISPER is ON. When it is off this file still compiles — it just hands
// back the null backend — so every caller links the same way and no #ifdef leaks past this TU.

namespace fl {

#if defined(FL_HAVE_WHISPER)

namespace {

// Whisper wants 16 kHz mono float. Anything else is resampled here rather than at the call site,
// because the capture device's rate is a HAL detail and the phrase matcher upstream has no business
// knowing about either.
constexpr int kWhisperSampleRate = 16000;

// Bound the queue. A stuck push-to-talk key, or a client shovelling audio faster than the CPU can
// transcribe, must not grow this without limit — dropping the newest with a logged refusal is
// honest, where unbounded growth is a slow memory leak that looks like a hang.
constexpr std::size_t kMaxQueuedUtterances = 4;

[[nodiscard]] std::vector<float> toWhisperPcm(const int16_t* samples, std::size_t count, int sampleRate) {
    std::vector<float> out;
    if (count == 0 || sampleRate <= 0)
        return out;
    if (sampleRate == kWhisperSampleRate) {
        out.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            out[i] = static_cast<float>(samples[i]) / 32768.0f;
        return out;
    }
    // Linear resample. Speech recognition is not sensitive enough to justify a windowed filter here,
    // and a simple one keeps this file free of a DSP dependency.
    const double ratio = static_cast<double>(kWhisperSampleRate) / static_cast<double>(sampleRate);
    const auto outCount = static_cast<std::size_t>(static_cast<double>(count) * ratio);
    out.resize(outCount);
    for (std::size_t i = 0; i < outCount; ++i) {
        const double src = static_cast<double>(i) / ratio;
        const auto i0 = static_cast<std::size_t>(src);
        const std::size_t i1 = i0 + 1 < count ? i0 + 1 : i0;
        const double frac = src - static_cast<double>(i0);
        const double v = static_cast<double>(samples[i0]) * (1.0 - frac) + static_cast<double>(samples[i1]) * frac;
        out[i] = static_cast<float>(v / 32768.0);
    }
    return out;
}

class WhisperSpeechToText final : public ISpeechToText {
  public:
    explicit WhisperSpeechToText(std::string modelPath) : m_modelPath(std::move(modelPath)) {}
    ~WhisperSpeechToText() override {
        shutdown();
    }

    bool init(ILogger& logger) override {
        m_logger = &logger;
        if (m_modelPath.empty()) {
            m_lastError = "no whisper model path configured";
            return false;
        }
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = false; // the whole point of this tier is that it does not need a GPU
        m_ctx = whisper_init_from_file_with_params(m_modelPath.c_str(), cparams);
        if (!m_ctx) {
            m_lastError = "failed to load whisper model '" + m_modelPath + "'";
            return false;
        }
        m_running = true;
        m_worker = std::thread([this] { workerLoop(); });
        return true;
    }

    void shutdown() override {
        if (!m_running.exchange(false))
            return;
        m_cv.notify_all();
        if (m_worker.joinable())
            m_worker.join();
        // Fire Cancelled for anything still queued, so a caller waiting on a transcript is never
        // left waiting forever (the IAsyncFilesystem contract).
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (const Job& j : m_queue)
                m_done.push_back({j.id, SttStatus::Cancelled, {}, {}});
            m_queue.clear();
        }
        service();
        if (m_ctx) {
            whisper_free(m_ctx);
            m_ctx = nullptr;
        }
    }

    void setHandler(ISpeechToTextHandler* handler) override {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_handler = handler;
    }

    SttRequestId submit(const int16_t* samples, std::size_t sampleCount, int sampleRate) override {
        if (!m_running || !samples || sampleCount == 0)
            return 0;
        std::vector<float> pcm = toWhisperPcm(samples, sampleCount, sampleRate);
        if (pcm.empty())
            return 0;
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_queue.size() >= kMaxQueuedUtterances) {
            if (m_logger)
                m_logger->log(LogLevel::Warn, __FILE__, __LINE__, "stt: transcription queue full; dropping utterance");
            return 0;
        }
        const SttRequestId id = ++m_nextId;
        m_queue.push_back({id, std::move(pcm)});
        m_cv.notify_one();
        return id;
    }

    void cancel(SttRequestId id) override {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->id == id) {
                m_queue.erase(it);
                m_done.push_back({id, SttStatus::Cancelled, {}, {}});
                return;
            }
        }
        // Already in flight: it completes normally. Cancellation is best-effort, as it is for
        // IAsyncFilesystem::cancelRead.
    }

    void service() override {
        std::vector<Completion> ready;
        ISpeechToTextHandler* handler = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            ready.swap(m_done);
            handler = m_handler;
        }
        // Handler fires OUTSIDE the lock: it will call back into the game, and holding a lock across
        // that is how a deadlock gets built.
        if (!handler)
            return;
        for (const Completion& c : ready)
            handler->onTranscript(c.id, c.status, c.text, c.error);
    }

    [[nodiscard]] bool available() const override {
        return m_ctx != nullptr;
    }

    [[nodiscard]] const char* getLastError() const override {
        return m_lastError.empty() ? nullptr : m_lastError.c_str();
    }

  private:
    struct Job {
        SttRequestId id;
        std::vector<float> pcm;
    };
    struct Completion {
        SttRequestId id;
        SttStatus status;
        std::string text;
        std::string error;
    };

    void workerLoop() {
        while (m_running) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [this] { return !m_running || !m_queue.empty(); });
                if (!m_running)
                    return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }

            whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            params.print_progress = false;
            params.print_realtime = false;
            params.print_timestamps = false;
            params.single_segment = true; // one push-to-talk press is one utterance
            params.translate = false;
            params.n_threads = 2; // a background transcribe must not fight the render thread

            Completion c{job.id, SttStatus::Success, {}, {}};
            if (whisper_full(m_ctx, params, job.pcm.data(), static_cast<int>(job.pcm.size())) != 0) {
                c.status = SttStatus::Error;
                c.error = "whisper_full failed";
            } else {
                const int segments = whisper_full_n_segments(m_ctx);
                for (int i = 0; i < segments; ++i)
                    c.text += whisper_full_get_segment_text(m_ctx, i);
            }
            std::lock_guard<std::mutex> lk(m_mutex);
            m_done.push_back(std::move(c));
        }
    }

    std::string m_modelPath;
    std::string m_lastError;
    ILogger* m_logger{nullptr};
    whisper_context* m_ctx{nullptr};

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    std::vector<Completion> m_done;
    ISpeechToTextHandler* m_handler{nullptr};
    SttRequestId m_nextId{0};
};

} // namespace

std::unique_ptr<ISpeechToText> createSpeechToText(const std::string& modelPath, ILogger& logger) {
    auto stt = std::make_unique<WhisperSpeechToText>(modelPath);
    if (stt->init(logger))
        return stt;
    const char* why = stt->getLastError();
    logger.log(LogLevel::Warn, __FILE__, __LINE__,
               (std::string("stt: ") + (why ? why : "init failed") +
                "; voice commands unavailable (the radio menu remains the path)")
                   .c_str());
    return std::make_unique<NullSpeechToText>();
}

#else // !FL_HAVE_WHISPER

std::unique_ptr<ISpeechToText> createSpeechToText(const std::string&, ILogger&) {
    // Lean build: one degradation path, same as createHttpClient's when libcurl is absent.
    return std::make_unique<NullSpeechToText>();
}

#endif

} // namespace fl
