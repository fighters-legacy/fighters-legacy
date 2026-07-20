// SPDX-License-Identifier: GPL-3.0-or-later
#include "IInput.h"
#include "IWindowEventHandler.h"
#include "Platform.h"
#include "SDL3Factory.h"
#include "VkRendererFactory.h"
#include "render/BuiltinGeometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

using namespace fl;

static std::atomic<bool> g_quit{false};
static void onSignal(int) {
    g_quit = true;
}

// One distinct color per BuiltinShape (Unknown, aircraft, missile, bomb, rocket,
// ground vehicle, naval vessel, structure, parachute) so the shape-row smoke reads at a glance.
static constexpr std::array<glm::vec4, 9> kShapeColors = {
    glm::vec4{1.00f, 0.15f, 0.90f, 1.0f}, // Unknown — magenta error beacon
    glm::vec4{0.15f, 0.40f, 1.00f, 1.0f}, // aircraft — blue
    glm::vec4{1.00f, 0.15f, 0.10f, 1.0f}, // missile — red
    glm::vec4{1.00f, 0.55f, 0.05f, 1.0f}, // bomb — orange
    glm::vec4{1.00f, 0.85f, 0.05f, 1.0f}, // rocket — yellow
    glm::vec4{0.10f, 0.80f, 0.20f, 1.0f}, // ground vehicle — green
    glm::vec4{0.10f, 0.85f, 0.90f, 1.0f}, // naval vessel — cyan
    glm::vec4{0.60f, 0.60f, 0.65f, 1.0f}, // structure — concrete grey
    glm::vec4{0.95f, 0.95f, 0.98f, 1.0f}, // parachute — canopy white
};

// Row placement + display scale per shape (the naval vessel is ~64 m; shrink the big
// ones so the whole set fits one orbit view).
static constexpr std::array<float, 9> kShapeScale = {1.0f, 0.6f, 1.0f, 1.0f, 1.0f, 0.8f, 0.12f, 0.35f, 1.0f};

class App : public IWindowEventHandler {
  public:
    explicit App(Platform& p) : m_platform(p) {}

    void onResize(int w, int h) override {
        m_platform.renderer->onResize(w, h);
    }
    void onClose() override {
        m_running = false;
    }

    int run() {
        if (!m_platform.window->init("Fighters Legacy — Hello Triangle", 1280, 720)) {
            std::fprintf(stderr, "window init failed: %s\n", m_platform.window->getLastError());
            return 1;
        }
        m_platform.window->setEventHandler(this);

        if (!m_platform.renderer->init(m_platform.window.get())) {
            std::fprintf(stderr, "renderer init failed: %s\n", m_platform.renderer->getLastError());
            return 1;
        }
        std::fprintf(stderr, "GPU: %s\n", m_platform.renderer->gpuInfo());

        // Upload every builtin placeholder shape (#886) — a strictly better renderer smoke
        // than the old 4 tetrahedron faces: it verifies each shape uploads, renders, and is
        // wound correctly (an inside-out shape shows its back faces / vanishes).
        for (int i = 0; i < kShapeCount; ++i) {
            char name[40];
            std::snprintf(name, sizeof(name), "builtin:shape%d", i);
            m_shapeMesh[i] =
                m_platform.renderer->createMesh({name, fl::builtinShapeGlb(static_cast<fl::BuiltinShape>(i))});
            std::fprintf(stderr, "shape%d mesh id=%-3u valid=%d\n", i, m_shapeMesh[i].id, (int)m_shapeMesh[i].valid());

            MaterialDesc md{};
            md.baseColorFactor = kShapeColors[i];
            md.roughnessFactor = 0.5f;
            m_shapeMat[i] = m_platform.renderer->createMaterial(md);
        }

        // Axis arrow heads: small darts at the end of each axis.
        // X=red, Y=green, Z=blue. Nose points +X by default; rotate for Y and Z.
        m_axisMesh =
            m_platform.renderer->createMesh({"builtin:axis-dart", fl::builtinShapeGlb(fl::BuiltinShape::Rocket)});
        const glm::vec4 kAxisColors[3] = {
            {1.0f, 0.1f, 0.1f, 1.0f}, // X — red
            {0.1f, 1.0f, 0.1f, 1.0f}, // Y — green
            {0.1f, 0.3f, 1.0f, 1.0f}, // Z — blue
        };
        for (int i = 0; i < 3; ++i) {
            MaterialDesc md{};
            md.baseColorFactor = kAxisColors[i];
            md.roughnessFactor = 0.4f;
            m_axisMat[i] = m_platform.renderer->createMaterial(md);
        }

        m_env.sunDirection = glm::normalize(glm::vec3{0.6f, 1.0f, 0.4f});

        auto fpsTimer = std::chrono::steady_clock::now();
        int fpsFrameCount = 0;

        while (m_running && !m_platform.window->shouldClose() && !g_quit) {
            m_platform.window->pollEvents();
            m_platform.renderer->beginFrame();

            // FPS counter — update title every 0.5 s.
            ++fpsFrameCount;
            const auto now = std::chrono::steady_clock::now();
            const float fpsDt = std::chrono::duration<float>(now - fpsTimer).count();
            if (fpsDt >= 0.5f) {
                const float fps = static_cast<float>(fpsFrameCount) / fpsDt;
                char title[96];
                std::snprintf(title, sizeof(title),
                              "Fighters Legacy — Hello Triangle  |  %.0f FPS"
                              "  |  LMB-drag: orbit   scroll/=/- : zoom   R: reset",
                              fps);
                m_platform.window->setTitle(title);
                fpsTimer = now;
                fpsFrameCount = 0;
            }

            IInput& in = *m_platform.input;

            // Scroll wheel zoom.
            {
                int scroll = in.getMouseScroll();
                if (scroll != 0) {
                    m_radius -= scroll * 1.5f;
                    m_radius = std::clamp(m_radius, 3.0f, 80.0f);
                }
            }

            // Mouse orbit — left button held while dragging adjusts yaw/pitch.
            if (in.isMouseButtonDown(MouseButton::Left)) {
                int dx, dy;
                in.getMouseDelta(dx, dy);
                m_yaw -= dx * 0.35f;
                m_pitch += dy * 0.25f;
                m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
            }

            // Keyboard zoom and reset.
            if (in.isKeyDown(Key::Equals))
                m_radius = std::max(3.0f, m_radius - 0.15f);
            if (in.isKeyDown(Key::Minus))
                m_radius = std::min(80.0f, m_radius + 0.15f);
            if (in.isKeyJustPressed(Key::R)) {
                m_yaw = 0.0f;
                m_pitch = -10.0f;
                m_radius = 40.0f;
            }
            if (in.isKeyJustPressed(Key::Escape))
                m_running = false;

            in.flush();

            // Spherical orbit: yaw rotates around Y, pitch raises/lowers the camera.
            const float yawRad = glm::radians(m_yaw);
            const float pitchRad = glm::radians(m_pitch);
            const glm::vec3 eye{
                m_radius * std::cos(pitchRad) * std::sin(yawRad),
                m_radius * std::sin(pitchRad),
                m_radius * std::cos(pitchRad) * std::cos(yawRad),
            };

            // Infinite reverse-Z perspective (proj[3][2] = near, proj[1][1] = -f for Vulkan Y-flip).
            const float fovY = 1.0472f; // 60°
            const int wi = m_platform.window->width();
            const int hi = m_platform.window->height();
            const float aspect = static_cast<float>(wi) / static_cast<float>(hi > 0 ? hi : 1);
            const float near = 0.1f;
            const float f = 1.0f / std::tan(fovY * 0.5f);
            glm::mat4 proj{0.0f};
            proj[0][0] = f / aspect;
            proj[1][1] = -f;
            proj[2][3] = -1.0f;
            proj[3][2] = near;

            const CameraView cam{glm::lookAt(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), proj, glm::dvec3(eye)};

            // 8 shape RenderItems + 3 axis arrow heads — must stay alive until endFrame() returns.
            std::array<RenderItem, kShapeCount + 3> items{};
            for (int i = 0; i < kShapeCount; ++i) {
                items[i].mesh = m_shapeMesh[i];
                items[i].material = m_shapeMat[i];
                // Row along Z, centered on the origin; big shapes display-scaled to fit.
                const float z = (static_cast<float>(i) - (kShapeCount - 1) * 0.5f) * 9.0f;
                items[i].transform = glm::translate(glm::mat4(1.f), {0.f, 0.f, z}) *
                                     glm::scale(glm::mat4(1.f), glm::vec3(kShapeScale[i]));
            }

            // Axis arrow heads: small darts (scale 1.6 of the ~1.6 m rocket) at 3.5 m along
            // each axis. X already points +X; Y rotates 90° around +Z; Z rotates -90° around +Y.
            const float as = 1.6f; // arrow scale
            const float ad = 3.5f; // distance along axis
            const int ax = kShapeCount;

            items[ax + 0].mesh = m_axisMesh;
            items[ax + 0].material = m_axisMat[0]; // X — red
            items[ax + 0].transform =
                glm::translate(glm::mat4(1.f), {ad, 0.f, 0.f}) * glm::scale(glm::mat4(1.f), glm::vec3(as));

            items[ax + 1].mesh = m_axisMesh;
            items[ax + 1].material = m_axisMat[1]; // Y — green
            items[ax + 1].transform = glm::translate(glm::mat4(1.f), {0.f, ad, 0.f}) *
                                      glm::rotate(glm::mat4(1.f), glm::radians(90.f), {0.f, 0.f, 1.f}) *
                                      glm::scale(glm::mat4(1.f), glm::vec3(as));

            items[ax + 2].mesh = m_axisMesh;
            items[ax + 2].material = m_axisMat[2]; // Z — blue
            items[ax + 2].transform = glm::translate(glm::mat4(1.f), {0.f, 0.f, ad}) *
                                      glm::rotate(glm::mat4(1.f), glm::radians(-90.f), {0.f, 1.f, 0.f}) *
                                      glm::scale(glm::mat4(1.f), glm::vec3(as));

            FrameScene scene{cam, std::span<const RenderItem>{items.data(), items.size()}, m_env, {}};
            m_platform.renderer->setScene(scene);

            m_platform.renderer->endFrame();
        }

        m_platform.renderer->shutdown();
        m_platform.window->shutdown();
        return 0;
    }

  private:
    Platform& m_platform;
    static constexpr int kShapeCount = static_cast<int>(fl::BuiltinShape::Count);
    std::array<MeshHandle, kShapeCount> m_shapeMesh{};
    std::array<MaterialHandle, kShapeCount> m_shapeMat{};
    MeshHandle m_axisMesh{};
    std::array<MaterialHandle, 3> m_axisMat{};
    EnvironmentState m_env{};
    float m_yaw{0.0f};
    float m_pitch{-10.0f};
    float m_radius{40.0f};
    bool m_running{true};
};

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    Platform p;
    auto wi = createSDL3WindowInput();
    p.window = std::move(wi.window);
    p.input = std::move(wi.input);
    p.display = createSDL3Display();
    p.renderer = createVulkanRenderer();
    return App(p).run();
}
