// SPDX-License-Identifier: GPL-3.0-or-later
#include "IWindowEventHandler.h"
#include "Platform.h"
#include "SDL3Display.h"
#include "SDL3Window.h"
#include "VkRendererFactory.h"
#include "render/BuiltinGeometry.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

static std::atomic<bool> g_quit{false};
static void onSignal(int) {
    g_quit = true;
}

// Face colors: red, green, blue, yellow.
static constexpr std::array<glm::vec4, 4> kFaceColors = {
    glm::vec4{1.0f, 0.15f, 0.10f, 1.0f},
    glm::vec4{0.10f, 0.80f, 0.20f, 1.0f},
    glm::vec4{0.15f, 0.40f, 1.00f, 1.0f},
    glm::vec4{1.00f, 0.85f, 0.05f, 1.0f},
};

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

        // Upload 4 single-face meshes, one per tetrahedron face.
        for (int i = 0; i < 4; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "builtin:face%d", i);
            m_faceMesh[i] = m_platform.renderer->createMesh({name, fl::builtinTetrahedronFaceGlb(i)});
            std::fprintf(stderr, "face%d mesh id=%-3u valid=%d\n", i, m_faceMesh[i].id, (int)m_faceMesh[i].valid());

            MaterialDesc md{};
            md.baseColorFactor = kFaceColors[i];
            md.roughnessFactor = 0.5f;
            m_faceMat[i] = m_platform.renderer->createMaterial(md);
        }

        // Axis arrow heads: small tetrahedra at the end of each axis.
        // X=red, Y=green, Z=blue. Nose points +X by default; rotate for Y and Z.
        m_axisMesh = m_platform.renderer->createMesh({"builtin:axis-tet", fl::builtinTetrahedronGlb()});
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
            // Extract wheel events before pollEvents() drains the SDL queue.
            SDL_PumpEvents();
            {
                SDL_Event ev;
                while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_WHEEL) > 0) {
                    m_radius -= ev.wheel.y * 1.5f;
                    m_radius = std::clamp(m_radius, 3.0f, 80.0f);
                }
            }
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

            // Mouse orbit — left button held while dragging adjusts yaw/pitch.
            {
                float mx = 0, my = 0;
                SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
                if (!m_firstFrame && (mb & SDL_BUTTON_LMASK)) {
                    m_yaw -= (mx - m_lastMouseX) * 0.35f;   // drag right → rotate right
                    m_pitch += (my - m_lastMouseY) * 0.25f; // drag down → tilt down
                    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
                }
                m_lastMouseX = mx;
                m_lastMouseY = my;
                m_firstFrame = false;
            }

            // Keyboard zoom and reset.
            {
                const bool* keys = SDL_GetKeyboardState(nullptr);
                if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
                    m_radius = std::max(3.0f, m_radius - 0.15f);
                if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
                    m_radius = std::min(80.0f, m_radius + 0.15f);
                if (keys[SDL_SCANCODE_R]) {
                    m_yaw = 0.0f;
                    m_pitch = -10.0f;
                    m_radius = 18.0f;
                }
                if (keys[SDL_SCANCODE_ESCAPE])
                    m_running = false;
            }

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

            const CameraView cam{glm::lookAt(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}), proj, eye};

            // 4 face RenderItems + 3 axis arrow heads — must stay alive until endFrame() returns.
            std::array<RenderItem, 7> items{};
            for (int i = 0; i < 4; ++i) {
                items[i].mesh = m_faceMesh[i];
                items[i].material = m_faceMat[i];
                items[i].transform = glm::mat4(1.0f);
            }

            // Axis arrow heads: small tetrahedra (scale 0.28) at 3.5 m along each axis.
            // X already points +X; Y rotates 90° around +Z; Z rotates -90° around +Y.
            const float as = 0.28f; // arrow scale
            const float ad = 3.5f;  // distance along axis

            items[4].mesh = m_axisMesh;
            items[4].material = m_axisMat[0]; // X — red
            items[4].transform =
                glm::translate(glm::mat4(1.f), {ad, 0.f, 0.f}) * glm::scale(glm::mat4(1.f), glm::vec3(as));

            items[5].mesh = m_axisMesh;
            items[5].material = m_axisMat[1]; // Y — green
            items[5].transform = glm::translate(glm::mat4(1.f), {0.f, ad, 0.f}) *
                                 glm::rotate(glm::mat4(1.f), glm::radians(90.f), {0.f, 0.f, 1.f}) *
                                 glm::scale(glm::mat4(1.f), glm::vec3(as));

            items[6].mesh = m_axisMesh;
            items[6].material = m_axisMat[2]; // Z — blue
            items[6].transform = glm::translate(glm::mat4(1.f), {0.f, 0.f, ad}) *
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
    std::array<MeshHandle, 4> m_faceMesh{};
    std::array<MaterialHandle, 4> m_faceMat{};
    MeshHandle m_axisMesh{};
    std::array<MaterialHandle, 3> m_axisMat{};
    EnvironmentState m_env{};
    float m_yaw{0.0f};
    float m_pitch{-10.0f};
    float m_radius{18.0f};
    float m_lastMouseX{0.0f};
    float m_lastMouseY{0.0f};
    bool m_firstFrame{true};
    bool m_running{true};
};

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    Platform p;
    p.window = std::make_unique<SDL3Window>();
    p.display = std::make_unique<SDL3Display>();
    p.renderer = createVulkanRenderer();
    return App(p).run();
}
