// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/PreviewScene.h"

#include <string>
#include <vector>

namespace fl {

// Parsed fl-viewer command line (#836). Pure data + a pure parser so the CLI surface is unit-testable
// with no renderer.
struct ViewerOptions {
    // Model source (mutually exclusive; both empty = builtin placeholder). `glbPath` is a positional
    // filesystem path to a bare .glb; `entityId` is a "pack:id" resolved through the mod stack.
    std::string glbPath;
    std::string entityId;
    std::string assetsRoot; // root that holds mods/ (empty = FL_ASSETS_ROOT / cwd); pack modes

    // Headless snapshot (#666). snapshotPath non-empty selects snapshot mode; empty = interactive
    // (interactive is a stub until fl-viewer's window lands, #838).
    std::string snapshotPath;
    int width{1280};
    int height{720};
    int frames{3}; // frames rendered before the capture (lets settings/sky settle)
    bool damaged{false};
    bool requireContent{false}; // exit non-zero if the model falls back to the builtin placeholder
    PreviewDebugView view{PreviewDebugView::Shaded};
    float yawDeg{35.0f};
    float pitchDeg{18.0f};

    bool showHelp{false};
    bool showVersion{false};
};

// Result of parsing: ok==false means a usage error (message in `error`); showHelp/showVersion set the
// corresponding flag on `options` and return ok==true.
struct ViewerParseResult {
    bool ok{true};
    std::string error;
    ViewerOptions options;
};

// Parse argv[1..argc-1] (the program name is NOT included). Pure — no filesystem, no env.
[[nodiscard]] ViewerParseResult parseViewerOptions(const std::vector<std::string>& args);

// The --help usage text.
[[nodiscard]] const char* viewerUsage();

} // namespace fl
