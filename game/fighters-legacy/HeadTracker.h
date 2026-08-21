// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "math/Angles.h"

#include "config/HeadTrackingSettings.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <optional>

namespace fl {

// Head tracking via the opentrack UDP protocol (#927). A datagram is exactly 6 little-endian doubles:
// x, y, z (centimetres) then yaw, pitch, roll (degrees). The parse + smoothing/timeout logic lives in
// this header as pure functions so it is unit-testable without a socket; the UDP receive lives in the
// .cpp (the ServerQueryClient socket idiom).

struct RawHeadPose {
    double xCm, yCm, zCm, yawDeg, pitchDeg, rollDeg;
};

// nullopt unless len == 48. The engine wire layer already assumes a little-endian host, so a straight
// memcpy of the six doubles matches on the target platforms.
[[nodiscard]] inline std::optional<RawHeadPose> parseOpentrackDatagram(const void* data, std::size_t len) {
    if (len != 6u * sizeof(double))
        return std::nullopt;
    RawHeadPose p;
    std::memcpy(&p, data, sizeof(p));
    return p;
}

// The smoothed, config-scaled head pose fed to the camera.
struct HeadPose {
    float yawRad{0.0f}, pitchRad{0.0f}, rollRad{0.0f};
    glm::vec3 offsetM{0.0f}; // aircraft-body frame: x fwd, y up, z right
    bool fresh{false};       // a packet was seen within kFreshTimeoutS
};

// Pure EMA filter + freshness clock. `raw == nullptr` means no packet this frame; dt advances the
// staleness clock. Unit-testable without a socket.
struct HeadPoseFilter {
    static constexpr float kFreshTimeoutS = 0.5f;

    void update(const RawHeadPose* raw, float dt, const HeadTrackingSettings& cfg) {
        if (raw) {
            // Map to the game's cockpit-look convention. opentrack yaw+ = head turning left; the game's
            // cockpit yaw+ also looks left, so a straight sign works (invertYaw fixes nonconforming rigs).
            const float sy = (cfg.invertYaw ? -1.0f : 1.0f) * cfg.yawScale;
            const float sp = (cfg.invertPitch ? -1.0f : 1.0f) * cfg.pitchScale;
            const float sr = (cfg.invertRoll ? -1.0f : 1.0f) * cfg.rollScale;
            const HeadPose target{static_cast<float>(raw->yawDeg) * fl::kDegToRad<float> * sy,
                                  static_cast<float>(raw->pitchDeg) * fl::kDegToRad<float> * sp,
                                  static_cast<float>(raw->rollDeg) * fl::kDegToRad<float> * sr,
                                  clampOffset(glm::vec3{-static_cast<float>(raw->zCm), static_cast<float>(raw->yCm),
                                                        static_cast<float>(raw->xCm)} *
                                              0.01f * cfg.positionalScale),
                                  true};
            const float s = (cfg.smoothing < 0.0f) ? 0.0f : (cfg.smoothing > 0.95f ? 0.95f : cfg.smoothing);
            const float alpha = (s <= 0.0f) ? 1.0f : (1.0f - std::pow(s, dt * 60.0f));
            pose.yawRad = lerp(pose.yawRad, target.yawRad, alpha);
            pose.pitchRad = lerp(pose.pitchRad, target.pitchRad, alpha);
            pose.rollRad = lerp(pose.rollRad, target.rollRad, alpha);
            pose.offsetM = glm::mix(pose.offsetM, target.offsetM, alpha);
            m_sinceLastPacketS = 0.0f;
        } else {
            m_sinceLastPacketS += dt;
        }
        pose.fresh = m_sinceLastPacketS < kFreshTimeoutS;
    }

    HeadPose pose{};

  private:
    static float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }
    static glm::vec3 clampOffset(glm::vec3 v) {
        constexpr float kMax = 0.5f; // ±0.5 m of head translation
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] < -kMax ? -kMax : (v[i] > kMax ? kMax : v[i]);
        return v;
    }
    float m_sinceLastPacketS{1e9f};
};

// The UDP receiver. Opens a non-blocking localhost socket on start(); poll() drains to the latest
// datagram then filters. Main-thread only. Implemented in HeadTracker.cpp.
class HeadTracker {
  public:
    HeadTracker() = default;
    ~HeadTracker();
    HeadTracker(const HeadTracker&) = delete;
    HeadTracker& operator=(const HeadTracker&) = delete;

    bool start(uint16_t port);
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return m_running;
    }
    // Drain recvfrom to the LATEST valid datagram, then filter.update(latestOrNull, dt, cfg).
    void poll(float dt, const HeadTrackingSettings& cfg);
    [[nodiscard]] const HeadPose& pose() const noexcept {
        return m_filter.pose;
    }

  private:
    HeadPoseFilter m_filter;
    bool m_running{false};
#if defined(_WIN32)
    unsigned long long m_sock{~0ull};
    bool m_wsaOwner{false};
#else
    int m_sock{-1};
#endif
};

} // namespace fl
