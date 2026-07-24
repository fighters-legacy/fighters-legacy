// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/PreviewScene.h"

namespace fl {

class AssetManager;
class ILogger;
struct ViewerOptions;

// Render the model headlessly and write the snapshot PNG (#666). `assets` may be null (bare-.glb
// mode). Returns a process exit code: 0 = PNG written, 1 = a failure (renderer init, capture, or
// --require-content with a builtin fallback).
int runSnapshot(const ViewerOptions& opts, PreviewScene::ModelDesc model, AssetManager* assets, ILogger& logger);

} // namespace fl
