// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>

namespace fl {

// Reference-counted init/shutdown of a PROCESS-GLOBAL third-party library (#1265).
//
// enet6, GameNetworkingSockets and libcurl all expose one process-wide init and one process-wide
// teardown, none of them reference-counted. Whenever two instances of a backend coexist — the
// bot_swarm harness runs N client hosts in one process, with staggered lifetimes — one instance's
// shutdown must not tear the library down under the others. All three backends grew the same
// counter to say so, and one of them grew it WRONG:
//
//     if (g_curlRefCount.fetch_add(1) == 0) { curl_global_init(...); }
//
// An atomic counter does not order the init against the observers of the count. A second thread
// whose fetch_add returns 1 proceeds into a library that the first thread is still initialising, and
// a failed init that fetch_sub's back can leave a third caller believing init already succeeded.
// Not reachable today — exactly one CurlHttpClient exists per process and it is built on the main
// thread — but it is the shape of the bug, not the schedule, that makes it worth removing.
//
// A mutex held ACROSS the init call is what excludes all of that: nobody observes the count until
// the library behind it is actually ready.
//
// Home is platform/ root (platform-hal) because platform/net and platform/http are siblings —
// neither can host a header the other needs.
class GlobalLibRef {
  public:
    // Initialise on the first successful acquire; every later acquire just takes a reference.
    // `init` runs UNDER the lock and returns false to report failure, in which case the count is
    // unchanged and this returns false too — a failed init hands out no reference.
    template <class InitFn> [[nodiscard]] bool acquire(InitFn&& init) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_count == 0 && !init())
            return false;
        ++m_count;
        return true;
    }

    // Release one reference, tearing the library down on the last. `shutdown` runs under the lock,
    // for the same reason `init` does.
    //
    // Also the unwind path for a caller whose acquire SUCCEEDED but whose own follow-up setup then
    // failed (GnsNetwork: init ok, SteamNetworkingSockets() returned null) — release immediately and
    // the library goes away iff this was the only reference.
    template <class ShutdownFn> void release(ShutdownFn&& shutdown) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_count > 0 && --m_count == 0)
            shutdown();
    }

  private:
    std::mutex m_mutex;
    int m_count{0};
};

} // namespace fl
