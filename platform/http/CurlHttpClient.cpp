// SPDX-License-Identifier: GPL-3.0-or-later
#include "http/CurlHttpClient.h"

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fl {

namespace {
std::atomic<int> g_curlRefCount{0}; // curl_global_init/cleanup refcount, mirrors ENet's g_enetRefCount
}

struct CurlHttpClient::Impl {
    explicit Impl(ILogger* log) : logger(log) {}

    ILogger* logger{nullptr};
    IHttpClientHandler* handler{nullptr};
    std::string lastError;

    // A queued request.
    struct Request {
        HttpRequestId id{0};
        HttpRequestOptions opts;
    };

    // An event to deliver on the main thread in service().
    struct Event {
        enum class Kind { Data, Progress, Complete } kind;
        HttpRequestId id{0};
        std::vector<uint8_t> data; // Kind::Data
        uint64_t received{0};      // Kind::Progress
        uint64_t total{0};         // Kind::Progress
        HttpStatus status{HttpStatus::Success};
        long httpCode{0};
        std::string errorMsg;
    };

    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;      // worker waits for requests; producer waits for queue drain
    std::condition_variable cvDrain; // main -> worker: the event queue has room again
    std::deque<Request> requests;
    std::deque<Event> events;
    std::unordered_set<HttpRequestId> cancelled;
    bool running{false};
    HttpRequestId nextId{1};

    static constexpr std::size_t kMaxQueuedEvents = 64; // backpressure: worker blocks past this

    void pushEvent(Event&& e) {
        std::unique_lock<std::mutex> lk(mtx);
        cvDrain.wait(lk, [&] { return !running || events.size() < kMaxQueuedEvents; });
        if (!running)
            return;
        events.push_back(std::move(e));
    }

    // curl write callback — runs on the worker thread. Copies the chunk into a Data event.
    static std::size_t writeCb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
        auto* ctx = static_cast<std::pair<Impl*, HttpRequestId>*>(userdata);
        const std::size_t n = size * nmemb;
        Event e;
        e.kind = Event::Kind::Data;
        e.id = ctx->second;
        e.data.assign(ptr, ptr + n);
        ctx->first->pushEvent(std::move(e));
        return n;
    }

    static int xferInfoCb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
        auto* ctx = static_cast<std::pair<Impl*, HttpRequestId>*>(userdata);
        Impl* self = ctx->first;
        {
            std::lock_guard<std::mutex> lk(self->mtx);
            if (self->cancelled.count(ctx->second))
                return 1; // non-zero aborts the transfer
        }
        Event e;
        e.kind = Event::Kind::Progress;
        e.id = ctx->second;
        e.received = static_cast<uint64_t>(dlnow < 0 ? 0 : dlnow);
        e.total = static_cast<uint64_t>(dltotal < 0 ? 0 : dltotal);
        self->pushEvent(std::move(e));
        return 0;
    }

    void runOne(const Request& req) {
        std::pair<Impl*, HttpRequestId> ctx{this, req.id};
        Event done;
        done.kind = Event::Kind::Complete;
        done.id = req.id;

        {
            std::lock_guard<std::mutex> lk(mtx);
            if (cancelled.count(req.id)) {
                done.status = HttpStatus::Cancelled;
                pushEvent(std::move(done));
                return;
            }
        }

        CURL* h = curl_easy_init();
        if (!h) {
            done.status = HttpStatus::Error;
            done.errorMsg = "curl_easy_init failed";
            pushEvent(std::move(done));
            return;
        }

        curl_easy_setopt(h, CURLOPT_URL, req.opts.url.c_str());
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Impl::writeCb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, &Impl::xferInfoCb);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, req.opts.maxRedirects > 0 ? 1L : 0L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, static_cast<long>(req.opts.maxRedirects));
        // Security: verify the peer + host, and never downgrade https->http on redirect.
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https,http");
        if (!req.opts.userAgent.empty())
            curl_easy_setopt(h, CURLOPT_USERAGENT, req.opts.userAgent.c_str());
        if (req.opts.connectTimeoutMs > 0)
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.opts.connectTimeoutMs));
        if (req.opts.totalTimeoutMs > 0)
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.opts.totalTimeoutMs));
        if (req.opts.lowSpeedTimeS > 0) {
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(req.opts.lowSpeedLimitBps));
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, static_cast<long>(req.opts.lowSpeedTimeS));
        }

        const CURLcode rc = curl_easy_perform(h);
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(h);

        bool wasCancelled;
        {
            std::lock_guard<std::mutex> lk(mtx);
            wasCancelled = cancelled.count(req.id) != 0;
        }
        if (rc == CURLE_ABORTED_BY_CALLBACK || wasCancelled) {
            done.status = HttpStatus::Cancelled;
        } else if (rc != CURLE_OK) {
            done.status = HttpStatus::Error;
            done.errorMsg = curl_easy_strerror(rc);
        } else {
            done.status = HttpStatus::Success;
            done.httpCode = code;
        }
        pushEvent(std::move(done));
    }

    void run() {
        for (;;) {
            Request req;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return !running || !requests.empty(); });
                if (!running && requests.empty())
                    return;
                req = std::move(requests.front());
                requests.pop_front();
            }
            runOne(req);
        }
    }
};

CurlHttpClient::CurlHttpClient(ILogger* log) : m_impl(std::make_unique<Impl>(log)) {}

CurlHttpClient::~CurlHttpClient() {
    shutdown();
}

bool CurlHttpClient::init() {
    if (m_impl->running)
        return true;
    if (g_curlRefCount.fetch_add(1) == 0) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            g_curlRefCount.fetch_sub(1);
            m_impl->lastError = "curl_global_init failed";
            return false;
        }
    }
    m_impl->running = true;
    m_impl->worker = std::thread([this] { m_impl->run(); });
    return true;
}

void CurlHttpClient::shutdown() {
    if (!m_impl->running && !m_impl->worker.joinable())
        return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->running = false;
    }
    m_impl->cv.notify_all();
    m_impl->cvDrain.notify_all();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    // Deliver Cancelled for anything still queued as a request but not yet completed.
    if (m_impl->handler) {
        std::deque<Impl::Request> leftover;
        {
            std::lock_guard<std::mutex> lk(m_impl->mtx);
            leftover.swap(m_impl->requests);
        }
        for (const auto& r : leftover)
            m_impl->handler->onHttpComplete(r.id, HttpStatus::Cancelled, 0, nullptr);
    }
    if (g_curlRefCount.fetch_sub(1) == 1)
        curl_global_cleanup();
}

void CurlHttpClient::setEventHandler(IHttpClientHandler* handler) {
    m_impl->handler = handler;
}

HttpRequestId CurlHttpClient::get(const HttpRequestOptions& options) {
    if (!m_impl->running || options.url.empty())
        return 0;
    HttpRequestId id;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        id = m_impl->nextId++;
        m_impl->requests.push_back({id, options});
    }
    m_impl->cv.notify_one();
    return id;
}

void CurlHttpClient::cancel(HttpRequestId id) {
    if (id == 0)
        return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->cancelled.insert(id);
}

void CurlHttpClient::service() {
    std::deque<Impl::Event> batch;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        batch.swap(m_impl->events);
    }
    m_impl->cvDrain.notify_all(); // let a blocked worker enqueue more
    if (!m_impl->handler)
        return;
    for (auto& e : batch) {
        switch (e.kind) {
        case Impl::Event::Kind::Data:
            m_impl->handler->onHttpData(e.id, e.data.data(), e.data.size());
            break;
        case Impl::Event::Kind::Progress:
            m_impl->handler->onHttpProgress(e.id, e.received, e.total);
            break;
        case Impl::Event::Kind::Complete:
            m_impl->handler->onHttpComplete(e.id, e.status, e.httpCode,
                                            e.errorMsg.empty() ? nullptr : e.errorMsg.c_str());
            break;
        }
    }
}

const char* CurlHttpClient::getLastError() const {
    return m_impl->lastError.empty() ? nullptr : m_impl->lastError.c_str();
}

} // namespace fl
