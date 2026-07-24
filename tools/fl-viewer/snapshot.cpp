// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot.h"
#include "viewer_options.h"

#include "render/PreviewScene.h"

#include "ILogger.h"
#include "IRenderer.h"
#include "VkRendererFactory.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace fl {

int runSnapshot(const ViewerOptions& opts, PreviewScene::ModelDesc model, AssetManager* assets, ILogger& logger) {
    auto renderer = createVulkanRenderer();
    if (!renderer) {
        std::fprintf(stderr, "fl-viewer: no Vulkan renderer available\n");
        return 1;
    }
    if (!renderer->initHeadless(static_cast<uint32_t>(opts.width), static_cast<uint32_t>(opts.height))) {
        std::fprintf(stderr, "fl-viewer: headless renderer init failed: %s\n", renderer->getLastError());
        return 1;
    }

    // Deterministic settings for reproducible goldens: TAA jitter and eye adaptation are the two
    // nondeterminism sources, so force both off. Everything else (shadows, GTAO, sky LUT) is
    // deterministic per driver. Bloom stays ON: the tonemap pass samples the bloom buffer
    // unconditionally, so disabling bloom leaves that image unwritten (a validation error / garbage
    // read) — a pre-existing renderer quirk, sidestepped here rather than worked into the preview.
    RendererSettings rs{};
    rs.vsync = RendererVsyncMode::Off;
    rs.aaMode = RendererAAMode::Off;
    rs.autoExposure = false;
    rs.bloom = true;
    renderer->applySettings(rs);

    PreviewScene preview(*renderer, assets, &logger);
    const bool loaded = preview.load(std::move(model));
    if (opts.requireContent && !loaded) {
        std::fprintf(stderr, "fl-viewer: --require-content set but the model fell back to the builtin placeholder\n");
        renderer->shutdown();
        return 1;
    }
    preview.setDamaged(opts.damaged);
    preview.setDebugView(opts.view);

    PreviewOrbit orbit = frameBounds(preview.boundsMin(), preview.boundsMax());
    orbit.yawDeg = opts.yawDeg;
    orbit.pitchDeg = opts.pitchDeg;
    const float aspect = static_cast<float>(opts.width) / static_cast<float>(opts.height > 0 ? opts.height : 1);

    const int frames = opts.frames < 1 ? 1 : opts.frames;
    for (int i = 0; i < frames; ++i) {
        renderer->beginFrame();
        FrameScene scene{};
        scene.camera = previewCameraView(orbit, aspect);
        auto items = preview.buildItems(scene.camera.worldOrigin);
        scene.renderItems = items;
        scene.environment = preview.environment();
        renderer->setScene(scene);
        if (i == frames - 1) {
            if (!renderer->captureScreenshot(opts.snapshotPath.c_str()))
                std::fprintf(stderr, "fl-viewer: captureScreenshot request refused (headless capture unsupported?)\n");
        }
        renderer->endFrame();
    }

    renderer->shutdown();

    std::error_code ec;
    const auto sz = std::filesystem::file_size(opts.snapshotPath, ec);
    if (ec || sz == 0) {
        std::fprintf(stderr, "fl-viewer: snapshot '%s' was not written\n", opts.snapshotPath.c_str());
        return 1;
    }
    std::fprintf(stderr, "fl-viewer: wrote %s (%llux%llu, %llu bytes)%s\n", opts.snapshotPath.c_str(),
                 static_cast<unsigned long long>(opts.width), static_cast<unsigned long long>(opts.height),
                 static_cast<unsigned long long>(sz), loaded ? "" : " [builtin placeholder]");
    return 0;
}

} // namespace fl
