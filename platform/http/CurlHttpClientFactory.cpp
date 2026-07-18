// SPDX-License-Identifier: GPL-3.0-or-later
#include "http/CurlHttpClientFactory.h"

#ifdef FL_HAVE_CURL
#include "http/CurlHttpClient.h"

#include <curl/curl.h>
#endif

namespace fl {

std::unique_ptr<IHttpClient> createHttpClient(ILogger* log) {
#ifdef FL_HAVE_CURL
    return std::make_unique<CurlHttpClient>(log);
#else
    (void)log;
    return nullptr;
#endif
}

const char* httpBackendVersion() {
#ifdef FL_HAVE_CURL
    return curl_version();
#else
    return "none";
#endif
}

} // namespace fl
