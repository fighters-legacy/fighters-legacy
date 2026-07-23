// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl-viewer — a standalone preview for the game renderer (#836/#666/#838).
//
// This commit (#666) ships snapshot mode only: it loads one entity def or a bare .glb through the
// real content stack and renderer, renders it headlessly, and writes a PNG. The interactive window
// arrives with #838; running without --snapshot prints a notice and exits.

#include "entity_resolve.h"
#include "snapshot.h"
#include "viewer_options.h"

#include "StdoutLogger.h"
#include "Version.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace fl;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    ViewerParseResult parsed = parseViewerOptions(args);
    if (!parsed.ok) {
        std::fprintf(stderr, "fl-viewer: %s\n\n%s", parsed.error.c_str(), viewerUsage());
        return 2;
    }
    const ViewerOptions& opts = parsed.options;
    if (opts.showHelp) {
        std::fputs(viewerUsage(), stdout);
        return 0;
    }
    if (opts.showVersion) {
        std::printf("fl-viewer %s\n", FL_VERSION_STRING);
        return 0;
    }

    StdoutLogger logger;

    // Resolve the model. --entity resolves through the mod stack; a positional .glb loads from disk;
    // neither given renders the builtin placeholder (a zero-content "is my pipeline alive" smoke).
    PreviewScene::ModelDesc model;
    AssetManager* assets = nullptr;
    std::unique_ptr<ViewerContent> content;
    std::string error;

    if (!opts.entityId.empty()) {
        content = ViewerContent::load(opts.assetsRoot, logger);
        assets = &content->assets();
        if (!content->resolveEntity(opts.entityId, model, error)) {
            std::fprintf(stderr, "fl-viewer: %s\n", error.c_str());
            return 1;
        }
    } else if (!opts.glbPath.empty()) {
        if (!loadBareGlb(opts.glbPath, model, error)) {
            std::fprintf(stderr, "fl-viewer: %s\n", error.c_str());
            return 1;
        }
    } else {
        // No model: the builtin placeholder (also lets --assets packs supply textures if given).
        if (!opts.assetsRoot.empty()) {
            content = ViewerContent::load(opts.assetsRoot, logger);
            assets = &content->assets();
        }
    }

    if (opts.snapshotPath.empty()) {
        std::fprintf(stderr, "fl-viewer: interactive viewer mode lands with #838; pass --snapshot <out.png> "
                             "for headless capture (or --help).\n");
        return 2;
    }

    return runSnapshot(opts, std::move(model), assets, logger);
}
