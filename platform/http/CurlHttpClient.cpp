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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fl {

namespace {
std::atomic<int> g_curlRefCount{0}; // curl_global_init/cleanup refcount, mirrors ENet's g_enetRefCount
}

struct CurlHttpClient::Impl {
    explicit Impl(ILogger* log) : logger(log) {}

    ILogger* logger{nullptr};
    std::string lastError;

    // Per-request handler (#1083). Populated by request(), erased when that request's Complete event is
    // delivered -- so a handler outlives exactly the requests it owns and no longer. Guarded by mtx
    // because request() runs on the caller's thread while service() drains on the main one.
    std::unordered_map<HttpRequestId, IHttpClientHandler*> handlers;

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

        // HTTP method + body (#143). GET is the default; POST/PUT carry the body, DELETE is method-only.
        switch (req.opts.method) {
        case HttpMethod::Get:
            curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::Post:
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.opts.body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.opts.body.size()));
            break;
        case HttpMethod::Put:
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.opts.body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.opts.body.size()));
            break;
        case HttpMethod::Delete_:
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        }
        struct curl_slist* headers = nullptr;
        std::string ctHeader;
        if (!req.opts.contentType.empty()) {
            ctHeader = "Content-Type: " + req.opts.contentType;
            headers = curl_slist_append(nullptr, ctHeader.c_str());
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        }

        const CURLcode rc = curl_easy_perform(h);
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(h);
        if (headers)
            curl_slist_free_all(headers);

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
    // Deliver Cancelled for anything still queued as a request but not yet completed. Routed per
    // request (#1083): shutdown() must tell EACH owner about ITS requests, not tell one handler about
    // everybody's. A request whose owner already called cancelRequestsFor() has no mapping left, so it
    // is correctly silent.
    {
        std::deque<Impl::Request> leftover;
        std::unordered_map<HttpRequestId, IHttpClientHandler*> owners;
        {
            std::lock_guard<std::mutex> lk(m_impl->mtx);
            leftover.swap(m_impl->requests);
            owners.swap(m_impl->handlers);
        }
        for (const auto& r : leftover) {
            const auto it = owners.find(r.id);
            if (it != owners.end() && it->second)
                it->second->onHttpComplete(r.id, HttpStatus::Cancelled, 0, nullptr);
        }
    }
    if (g_curlRefCount.fetch_sub(1) == 1)
        curl_global_cleanup();
}

HttpRequestId CurlHttpClient::request(const HttpRequestOptions& options, IHttpClientHandler* handler) {
    // A null handler is refused rather than accepted-and-dropped: a request whose completion goes nowhere
    // is the silent failure this change exists to remove.
    if (!m_impl->running || options.url.empty() || handler == nullptr)
        return 0;
    HttpRequestId id;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        id = m_impl->nextId++;
        m_impl->requests.push_back({id, options});
        m_impl->handlers.emplace(id, handler);
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

void CurlHttpClient::cancelRequestsFor(IHttpClientHandler* handler) {
    if (!handler)
        return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (auto it = m_impl->handlers.begin(); it != m_impl->handlers.end();) {
        if (it->second == handler) {
            m_impl->cancelled.insert(it->first);
            it = m_impl->handlers.erase(it); // no mapping left, so service() delivers nothing
        } else {
            ++it;
        }
    }
    // Events already queued for those ids are dropped by service()'s "no handler" branch.
}

void CurlHttpClient::service() {
    std::deque<Impl::Event> batch;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        batch.swap(m_impl->events);
    }
    m_impl->cvDrain.notify_all(); // let a blocked worker enqueue more

    for (auto& e : batch) {
        // Route by request id (#1083). A handler is looked up per event rather than held across the
        // batch, because a Complete for one request may run before a Data for another.
        IHttpClientHandler* handler = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_impl->mtx);
            const auto it = m_impl->handlers.find(e.id);
            if (it != m_impl->handlers.end())
                handler = it->second;
            // The completion is the last event for a request, so its handler mapping retires with it.
            if (e.kind == Impl::Event::Kind::Complete && it != m_impl->handlers.end())
                m_impl->handlers.erase(it);
        }
        if (!handler)
            continue; // a cancelled-and-forgotten request; nothing to deliver it to

        switch (e.kind) {
        case Impl::Event::Kind::Data:
            // onHttpData's documented contract is "return false to ABORT the transfer", and the return
            // value used to be discarded here -- an advertised control that did nothing. Honour it by
            // cancelling, which is exactly what the doc says happens (the transfer then completes as
            // Cancelled, so the handler still gets its terminal callback).
            if (!handler->onHttpData(e.id, e.data.data(), e.data.size()))
                cancel(e.id);
            break;
        case Impl::Event::Kind::Progress:
            handler->onHttpProgress(e.id, e.received, e.total);
            break;
        case Impl::Event::Kind::Complete:
            handler->onHttpComplete(e.id, e.status, e.httpCode, e.errorMsg.empty() ? nullptr : e.errorMsg.c_str());
            break;
        }
    }
}

const char* CurlHttpClient::getLastError() const {
    return m_impl->lastError.empty() ? nullptr : m_impl->lastError.c_str();
}

} // namespace fl
