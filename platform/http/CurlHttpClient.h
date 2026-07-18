// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IHttpClient.h"
#include "ILogger.h"

#include <memory>

// libcurl IHttpClient backend (#490). curl types are kept out of this header (pimpl), so consumers
// include only IHttpClient.h + the factory. Built only when find_package(CURL) succeeds
// (FL_HAVE_CURL); otherwise createCurlHttpClient() returns nullptr. TLS validation is always on and
// https->http redirect downgrades are always refused.

namespace fl {

class CurlHttpClient : public IHttpClient {
  public:
    explicit CurlHttpClient(ILogger* log = nullptr);
    ~CurlHttpClient() override;

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    bool init() override;
    void shutdown() override;
    void setEventHandler(IHttpClientHandler* handler) override;
    HttpRequestId get(const HttpRequestOptions& options) override;
    void cancel(HttpRequestId id) override;
    void service() override;
    const char* getLastError() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
