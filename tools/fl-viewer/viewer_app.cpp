// SPDX-License-Identifier: GPL-3.0-or-later
#include "viewer_app.h"
#include "entity_resolve.h"
#include "overlay_gizmos.h"
#include "viewer_options.h"

#include "render/PreviewScene.h"

#include "content/AssetManager.h"
#include "mesh_validator.h"

#include "IGui.h"
#include "IInput.h"
#include "ILogger.h"
#include "IRenderer.h"
#include "IWindow.h"
#include "IWindowEventHandler.h"
#include "ImGuiGui.h" // createImGuiGui
#include "SDL3Factory.h"
#include "StdFilesystemWatcher.h"
#include "VkRendererFactory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl {

namespace {

// Load the mesh bytes for the node-tree panel: the bare .glb bytes, or the pack mesh via AssetManager.
std::vector<uint8_t> loadMeshBytes(const PreviewScene::ModelDesc& model, AssetManager* assets) {
    if (!model.glbBytes.empty())
        return model.glbBytes;
    if (assets && !model.meshAssetName.empty()) {
        if (auto d = assets->loadMesh(model.meshAssetName.c_str()); d && !d->bytes.empty())
            return d->bytes;
    }
    return {};
}

class ViewerApp : public IWindowEventHandler {
  public:
    ViewerApp(const ViewerOptions& opts, PreviewScene::ModelDesc model, ViewerContent* content, AssetManager* assets,
              ILogger& logger)
        : m_opts(opts), m_model(std::move(model)), m_content(content), m_assets(assets), m_logger(logger) {}

    void onResize(int w, int h) override {
        if (m_renderer)
            m_renderer->onResize(w, h);
    }
    void onClose() override {
        m_running = false;
    }

    int run() {
        auto wi = createSDL3WindowInput();
        m_window = std::move(wi.window);
        m_input = std::move(wi.input);
        auto display = createSDL3Display();
        m_renderer = createVulkanRenderer();

        const std::string title =
            "fl-viewer — " + (m_opts.entityId.empty()
                                  ? (m_opts.glbPath.empty() ? std::string("builtin") : m_opts.glbPath)
                                  : m_opts.entityId);
        if (!m_window->init(title.c_str(), m_opts.width, m_opts.height)) {
            std::fprintf(stderr, "fl-viewer: window init failed: %s\n", m_window->getLastError());
            return 1;
        }
        m_window->setEventHandler(this);
        if (!m_renderer->init(m_window.get())) {
            std::fprintf(stderr, "fl-viewer: renderer init failed: %s\n", m_renderer->getLastError());
            return 1;
        }

        m_gui = createImGuiGui(*m_window, *m_renderer);
        if (m_gui) {
            IGui* gui = m_gui.get();
            m_window->setGuiEventForwarder([gui](const void* ev) { gui->processEvent(ev); });
        }

        m_preview = std::make_unique<PreviewScene>(*m_renderer, m_assets, &m_logger);
        loadModel();

        // Hot-reload watcher (#152/#838): always on in the viewer (its killer feature). Pack mode
        // watches the mod stack; bare-.glb mode watches the file's parent dir non-recursively.
        setupWatcher();

        std::array<glm::vec3, 0> noEmit{};
        (void)noEmit;
        while (m_running && !m_window->shouldClose()) {
            m_window->pollEvents();
            pollHotReload();

            if (m_gui)
                m_gui->newFrame();
            handleInput();
            if (m_gui)
                buildPanels();

            m_renderer->beginFrame();
            const float aspect = static_cast<float>(m_window->width()) /
                                 static_cast<float>(m_window->height() > 0 ? m_window->height() : 1);
            CameraView cam = previewCameraView(m_orbit, aspect);
            FrameScene scene{};
            scene.camera = cam;
            auto items = m_preview->buildItems(cam.worldOrigin);
            scene.renderItems = items;
            scene.environment = m_preview->environment();
            m_renderer->setScene(scene);

            // Grid + axis gizmos as 2D overlays (no renderer changes).
            m_overlay.clear();
            if (m_showGrid) {
                auto grid = buildGridOverlay(cam, autoGridSpacing(boundsRadius()), 10, {0.4f, 0.4f, 0.45f, 0.5f});
                m_overlay.insert(m_overlay.end(), grid.begin(), grid.end());
            }
            if (m_showAxes) {
                auto axes = buildAxisGizmo(cam, boundsRadius() * 1.3f + 0.5f);
                m_overlay.insert(m_overlay.end(), axes.begin(), axes.end());
            }
            if (!m_overlay.empty())
                m_renderer->submitOverlayElements(m_overlay);

            if (m_gui)
                m_gui->render();
            m_renderer->endFrame();
            m_input->flush();
        }

        m_preview.reset();
        m_renderer->shutdown();
        m_window->shutdown();
        return 0;
    }

  private:
    float boundsRadius() const {
        return 0.5f * glm::length(m_preview->boundsMax() - m_preview->boundsMin());
    }

    void applyView() {
        m_preview->setDebugView(m_wireframe   ? PreviewDebugView::Wireframe
                                : m_normals   ? PreviewDebugView::Normals
                                : m_faceColor ? PreviewDebugView::FaceColor
                                              : PreviewDebugView::Shaded);
        m_preview->setDamaged(m_damaged);
    }

    void loadModel() {
        m_preview->load(m_model);
        applyView();
        reframe();
        // Node tree + validate-mesh diagnostics for the panels.
        auto bytes = loadMeshBytes(m_model, m_assets);
        m_tree.reset();
        if (!bytes.empty())
            m_tree = describeMeshNodesFromMemory(bytes.data(), bytes.size());
        m_diag = MeshValidationResult{};
        if (!m_opts.glbPath.empty())
            m_diag = validateMesh(m_opts.glbPath); // path-based diagnostics for a bare .glb
    }

    void reframe() {
        PreviewOrbit framed = frameBounds(m_preview->boundsMin(), m_preview->boundsMax());
        m_orbit.focus = framed.focus;
        m_orbit.distance = framed.distance;
    }

    void reload() {
        m_preview->reload();
        applyView();
        auto bytes = loadMeshBytes(m_model, m_assets);
        if (!bytes.empty())
            m_tree = describeMeshNodesFromMemory(bytes.data(), bytes.size());
        if (!m_opts.glbPath.empty())
            m_diag = validateMesh(m_opts.glbPath);
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, "fl-viewer: reloaded");
    }

    void setupWatcher() {
        namespace fs = std::filesystem;
        if (m_content && m_assets) {
            m_watcher = std::make_unique<StdFilesystemWatcher>(
                fs::path(m_content->resolvedRoot()), fs::path(m_content->resolvedRoot()), 250, 20000, &m_logger);
            m_assets->enableHotReload(*m_watcher);
        } else if (!m_model.glbDir.empty()) {
            m_watcher = std::make_unique<StdFilesystemWatcher>(m_model.glbDir, m_model.glbDir, 250, 20000, &m_logger);
            m_watcher->watch(PathDomain::Assets, "", /*recursive=*/false); // the file's own directory
        }
    }

    void pollHotReload() {
        if (!m_watcher)
            return;
        bool changed = false;
        if (m_content && m_assets) {
            auto rep = m_assets->processHotReload();
            for (const auto& c : rep.changed) {
                if (c.type == AssetType::Mesh)
                    m_preview_needsReload = true; // re-read on the next frame boundary
                if (c.type == AssetType::Mesh || c.type == AssetType::Texture || c.type == AssetType::Livery)
                    changed = true;
            }
        } else {
            changed = !m_watcher->pollEvents().empty();
        }
        if (changed || m_preview_needsReload) {
            m_preview_needsReload = false;
            reload();
        }
    }

    void handleInput() {
        IInput& in = *m_input;
        const bool guiMouse = m_gui && m_gui->wantCaptureMouse();
        const bool guiKeys = m_gui && m_gui->wantCaptureKeyboard();

        if (!guiMouse) {
            if (int s = in.getMouseScroll(); s != 0)
                m_orbit.distance = std::clamp(m_orbit.distance * std::pow(0.87f, static_cast<float>(s)),
                                              0.25f * boundsRadius() + 0.1f, 50.0f * boundsRadius() + 5.0f);
            if (in.isMouseButtonDown(MouseButton::Left)) {
                int dx, dy;
                in.getMouseDelta(dx, dy);
                m_orbit.yawDeg -= dx * 0.35f;
                m_orbit.pitchDeg = std::clamp(m_orbit.pitchDeg + dy * 0.25f, -89.0f, 89.0f);
            }
            if (in.isMouseButtonDown(MouseButton::Right)) {
                int dx, dy;
                in.getMouseDelta(dx, dy);
                // Pan the focus in the view plane (approximate; scaled by distance).
                const float k = m_orbit.distance * 0.002f;
                m_orbit.focus.x -= dx * k;
                m_orbit.focus.y += dy * k;
            }
        }
        if (!guiKeys) {
            if (in.isKeyJustPressed(Key::F) || in.isKeyJustPressed(Key::R))
                reframe();
            if (in.isKeyJustPressed(Key::F5))
                reload();
            if (in.isKeyJustPressed(Key::Escape))
                m_running = false;
        }
    }

    void buildPanels() {
        if (!m_gui->beginWindow("fl-viewer", 0.0f, 0.0f, 0.26f, 1.0f)) {
            m_gui->endWindow();
            return;
        }
        // Model info.
        m_gui->label(m_opts.entityId.empty() ? (m_opts.glbPath.empty() ? "builtin placeholder" : m_opts.glbPath.c_str())
                                             : m_opts.entityId.c_str());
        char dims[96];
        const glm::vec3 sz = m_preview->boundsMax() - m_preview->boundsMin();
        std::snprintf(dims, sizeof(dims), "size: %.2f x %.2f x %.2f m", sz.x, sz.y, sz.z);
        m_gui->label(dims);
        m_gui->label(m_preview->loadedFromContent() ? "content mesh" : "builtin fallback (no content)");
        m_gui->separator();

        // View toggles.
        m_gui->label("View");
        bool w = m_wireframe, n = m_normals, f = m_faceColor, d = m_damaged, g = m_showGrid, a = m_showAxes;
        // Wireframe greyed when unsupported — reflected by leaving it off + a note.
        if (m_renderer->supportsWireframe()) {
            if (m_gui->checkbox("Wireframe", &w))
                setExclusiveView(w, m_normals, m_faceColor);
        } else {
            m_gui->label("(wireframe: unsupported GPU)");
        }
        if (m_gui->checkbox("Normals", &n))
            setExclusiveView(m_wireframe, n, m_faceColor);
        if (m_gui->checkbox("Face colors", &f))
            setExclusiveView(m_wireframe, m_normals, f);
        if (m_preview->hasDamageMesh()) {
            if (m_gui->checkbox("Damage variant", &d)) {
                m_damaged = d;
                applyView();
            }
        } else {
            m_gui->label("(no damage variant)");
        }
        if (m_gui->checkbox("Grid", &g))
            m_showGrid = g;
        if (m_gui->checkbox("Axes", &a))
            m_showAxes = a;
        if (m_gui->button("Reload (F5)"))
            reload();
        m_gui->separator();

        // Node tree.
        m_gui->label("Nodes");
        m_gui->label("engine draws meshes[0].primitives[0] only (#837)");
        if (m_tree)
            emitNodeRow(0, /*rootScan=*/true);
        m_gui->separator();

        // Diagnostics.
        m_gui->label("Diagnostics (validate-mesh)");
        if (m_opts.glbPath.empty() && !m_content)
            m_gui->label("(needs a file path)");
        for (const auto& e : m_diag.errors)
            m_gui->label(("ERR: " + e).c_str());
        for (const auto& wn : m_diag.warnings)
            m_gui->label(("WARN: " + wn).c_str());
        if (!m_opts.glbPath.empty() && m_diag.errors.empty() && m_diag.warnings.empty())
            m_gui->label("clean");

        m_gui->endWindow();
    }

    // Emit the node subtree rooted at each node whose parent is -1 (when rootScan) or the children of
    // `idx`. Simple recursive walk with treeNode/treePop.
    void emitNodeRow(int idx, bool rootScan) {
        if (!m_tree)
            return;
        if (rootScan) {
            for (int i = 0; i < static_cast<int>(m_tree->nodes.size()); ++i)
                if (m_tree->nodes[i].parent < 0)
                    emitNodeRow(i, false);
            return;
        }
        const MeshNodeInfo& node = m_tree->nodes[idx];
        std::string label = node.name.empty() ? ("node " + std::to_string(idx)) : node.name;
        if (node.damageVariant)
            label += " [_b]";
        if (!node.engineDrawn && node.meshIndex >= 0)
            label += " (not drawn)";
        char id[32];
        std::snprintf(id, sizeof(id), "n%d", idx);
        std::vector<int> children;
        for (int c = 0; c < static_cast<int>(m_tree->nodes.size()); ++c)
            if (m_tree->nodes[c].parent == idx)
                children.push_back(c);
        const bool leaf = children.empty();
        bool selected = (m_selectedNode == idx);
        if (m_gui->treeNode(id, label.c_str(), &selected, leaf)) {
            for (int c : children)
                emitNodeRow(c, false);
            m_gui->treePop();
        }
        if (selected)
            m_selectedNode = idx;
    }

    void setExclusiveView(bool w, bool n, bool f) {
        m_wireframe = w;
        m_normals = n && !w;
        m_faceColor = f && !w && !n;
        applyView();
    }

    ViewerOptions m_opts;
    PreviewScene::ModelDesc m_model;
    ViewerContent* m_content{nullptr};
    AssetManager* m_assets{nullptr};
    ILogger& m_logger;

    std::unique_ptr<IWindow> m_window;
    std::unique_ptr<IInput> m_input;
    std::unique_ptr<IRenderer> m_renderer;
    std::unique_ptr<IGui> m_gui;
    std::unique_ptr<PreviewScene> m_preview;
    std::unique_ptr<StdFilesystemWatcher> m_watcher;
    std::optional<MeshNodeTree> m_tree;
    MeshValidationResult m_diag;

    PreviewOrbit m_orbit;
    bool m_running{true};
    bool m_wireframe{false}, m_normals{false}, m_faceColor{false}, m_damaged{false};
    bool m_showGrid{true}, m_showAxes{true};
    bool m_preview_needsReload{false};
    int m_selectedNode{-1};
    std::vector<HudElement> m_overlay;
};

} // namespace

int runViewer(const ViewerOptions& opts, PreviewScene::ModelDesc model, ViewerContent* content, AssetManager* assets,
              ILogger& logger) {
    ViewerApp app(opts, std::move(model), content, assets, logger);
    return app.run();
}

} // namespace fl
