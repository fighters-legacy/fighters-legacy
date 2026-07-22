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
    void setEventHandler(IHttpClientHandler*) override {}
    HttpRequestId request(const HttpRequestOptions&) override {
        return 0;
    }
    void cancel(HttpRequestId) override {}
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
    void setEventHandler(IHttpClientHandler* h) override {
        m_handler = h;
    }
    HttpRequestId request(const HttpRequestOptions& o) override {
        if (o.url.empty())
            return 0;
        requestedUrls.push_back(o.url);
        requests.push_back(o);
        m_pending.push_back({m_nextId, o.url});
        return m_nextId++;
    }
    void cancel(HttpRequestId id) override {
        m_cancelled.push_back(id);
    }
    void service() override {
        auto pending = m_pending;
        m_pending.clear();
        for (const auto& [id, url] : pending) {
            const bool wasCancelled = std::find(m_cancelled.begin(), m_cancelled.end(), id) != m_cancelled.end();
            if (wasCancelled) {
                if (m_handler)
                    m_handler->onHttpComplete(id, HttpStatus::Cancelled, 0, nullptr);
                continue;
            }
            auto it = responses.find(url);
            if (it == responses.end()) {
                if (m_handler)
                    m_handler->onHttpComplete(id, HttpStatus::Error, 0, "no canned response");
                continue;
            }
            const Canned& c = it->second;
            bool aborted = false;
            if (m_handler && c.status == HttpStatus::Success) {
                for (const auto& chunk : c.chunks) {
                    if (!m_handler->onHttpData(id, chunk.data(), chunk.size())) {
                        aborted = true;
                        break;
                    }
                }
            }
            if (m_handler) {
                if (aborted)
                    m_handler->onHttpComplete(id, HttpStatus::Cancelled, 0, nullptr);
                else
                    m_handler->onHttpComplete(id, c.status, c.httpCode,
                                              c.status == HttpStatus::Error ? "canned error" : nullptr);
            }
        }
    }
    const char* getLastError() const override {
        return nullptr;
    }

  private:
    IHttpClientHandler* m_handler{nullptr};
    HttpRequestId m_nextId{1};
    std::deque<std::pair<HttpRequestId, std::string>> m_pending;
    std::vector<HttpRequestId> m_cancelled;
};

} // namespace fl
