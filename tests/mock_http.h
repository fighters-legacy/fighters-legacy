// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IHttpClient.h"

#include <algorithm>

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// HTTP HAL test doubles (#490). NullHttpClient = inert; TrackingHttpClient replays canned responses
// so ContentDownloader can be driven deterministically with no sockets. Naming mirrors
// mock_network.h's NullNetwork/TrackingNetwork.

namespace fl {

struct NullHttpClient : public IHttpClient {
    bool init() override {
        return true;
    }
    void shutdown() override {}
    HttpRequestId request(const HttpRequestOptions&, IHttpClientHandler*) override {
        return 0;
    }
    void cancel(HttpRequestId) override {}
    void cancelRequestsFor(IHttpClientHandler*) override {}
    void service() override {}
    const char* getLastError() const override {
        return nullptr;
    }
};

// Replays a per-URL canned body (optionally split into chunks) + HTTP code on service(). Unknown
// URLs complete as Error. Records every requested URL.
struct TrackingHttpClient : public IHttpClient {
    struct Canned {
        std::vector<std::string> chunks; // body, emitted as one onHttpData per chunk
        long httpCode{200};
        HttpStatus status{HttpStatus::Success};
    };

    std::unordered_map<std::string, Canned> responses;
    std::vector<std::string> requestedUrls;
    std::vector<HttpRequestOptions> requests; // full options of every request (#143: method/body/url)

    void setResponse(const std::string& url, const std::string& body, long code = 200, std::size_t chunkSize = 0) {
        Canned c;
        c.httpCode = code;
        if (chunkSize == 0 || body.empty()) {
            c.chunks.push_back(body);
        } else {
            for (std::size_t off = 0; off < body.size(); off += chunkSize)
                c.chunks.push_back(body.substr(off, chunkSize));
        }
        responses[url] = std::move(c);
    }

    bool init() override {
        return true;
    }
    void shutdown() override {}
    // Per-request handler (#1083): the mock routes by id exactly as the real backend does, which is what
    // lets a test drive two consumers at once and see that neither steals the other's completions.
    HttpRequestId request(const HttpRequestOptions& o, IHttpClientHandler* handler) override {
        if (o.url.empty() || handler == nullptr)
            return 0;
        requestedUrls.push_back(o.url);
        requests.push_back(o);
        m_pending.push_back({m_nextId, o.url});
        m_handlers[m_nextId] = handler;
        return m_nextId++;
    }
    void cancel(HttpRequestId id) override {
        m_cancelled.push_back(id);
    }
    void cancelRequestsFor(IHttpClientHandler* handler) override {
        for (auto it = m_handlers.begin(); it != m_handlers.end();) {
            if (it->second == handler) {
                m_cancelled.push_back(it->first);
                it = m_handlers.erase(it); // and deliver nothing: the owner is going away
            } else {
                ++it;
            }
        }
    }
    void service() override {
        auto pending = m_pending;
        m_pending.clear();
        for (const auto& [id, url] : pending) {
            const auto hit = m_handlers.find(id);
            IHttpClientHandler* const handler = (hit != m_handlers.end()) ? hit->second : nullptr;
            if (hit != m_handlers.end())
                m_handlers.erase(hit); // the completion below is this request's last event
            if (!handler)
                continue; // forgotten by cancelRequestsFor: its owner is gone

            const bool wasCancelled = std::find(m_cancelled.begin(), m_cancelled.end(), id) != m_cancelled.end();
            if (wasCancelled) {
                handler->onHttpComplete(id, HttpStatus::Cancelled, 0, nullptr);
                continue;
            }
            auto it = responses.find(url);
            if (it == responses.end()) {
                handler->onHttpComplete(id, HttpStatus::Error, 0, "no canned response");
                continue;
            }
            const Canned& c = it->second;
            bool aborted = false;
            if (c.status == HttpStatus::Success) {
                for (const auto& chunk : c.chunks) {
                    if (!handler->onHttpData(id, chunk.data(), chunk.size())) {
                        aborted = true;
                        break;
                    }
                }
            }
            if (aborted)
                handler->onHttpComplete(id, HttpStatus::Cancelled, 0, nullptr);
            else
                handler->onHttpComplete(id, c.status, c.httpCode,
                                        c.status == HttpStatus::Error ? "canned error" : nullptr);
        }
    }
    const char* getLastError() const override {
        return nullptr;
    }

  private:
    std::unordered_map<HttpRequestId, IHttpClientHandler*> m_handlers;
    HttpRequestId m_nextId{1};
    std::deque<std::pair<HttpRequestId, std::string>> m_pending;
    std::vector<HttpRequestId> m_cancelled;
};

} // namespace fl
