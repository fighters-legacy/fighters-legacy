// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IHttpClient.h"
#include "ILogger.h"

#include <memory>

// The HTTP HAL factory (#490). Consumers link `platform-http` and include only this — never
// CurlHttpClient.h. Returns a libcurl-backed client, or nullptr when the build has no libcurl
// (FL_HAVE_CURL off; e.g. a lean tools build). fl-server never creates one — it is a client concern.

namespace fl {

[[nodiscard]] std::unique_ptr<IHttpClient> createHttpClient(ILogger* log = nullptr);

// "libcurl/x.y.z ..." when a backend is compiled in, else "none".
[[nodiscard]] const char* httpBackendVersion();

} // namespace fl
