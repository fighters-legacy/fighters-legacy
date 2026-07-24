// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/PreviewScene.h"

namespace fl {

class AssetManager;
class ILogger;
class ViewerContent;
struct ViewerOptions;

// Open the interactive fl-viewer window (#838): the model on the game renderer with an orbit camera,
// a node-tree panel, view toggles (wireframe / normals / face-color / damage / grid / axes),
// validate-mesh diagnostics, and live hot-reload. `content`/`assets` are null in bare-.glb mode.
// Returns a process exit code.
int runViewer(const ViewerOptions& opts, PreviewScene::ModelDesc model, ViewerContent* content, AssetManager* assets,
              ILogger& logger);

} // namespace fl
