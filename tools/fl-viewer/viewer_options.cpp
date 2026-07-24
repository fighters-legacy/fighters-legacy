// SPDX-License-Identifier: GPL-3.0-or-later
#include "viewer_options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fl {

const char* viewerUsage() {
    return "fl-viewer — model preview for the game renderer\n"
           "\n"
           "Usage:\n"
           "  fl-viewer [<model.glb>] [--entity <pack:id>] [--assets <root>] [options]\n"
           "\n"
           "Model source (default: builtin placeholder):\n"
           "  <model.glb>            a bare .glb file (textures resolved relative to it)\n"
           "  --entity <pack:id>     an entity def id resolved through the mod stack (e.g. fl-base:f5e)\n"
           "  --assets <root>        content root that holds mods/ (default: $FL_ASSETS_ROOT or cwd)\n"
           "\n"
           "Headless snapshot (renders N frames, writes a PNG, exits):\n"
           "  --snapshot <out.png>   write a PNG and exit (required for headless mode)\n"
           "  --size WxH             render resolution (default 1280x720)\n"
           "  --frames N             frames to render before capture (default 3)\n"
           "  --damaged              show the damage-mesh variant\n"
           "  --view <mode>          shaded | facecolor (wireframe/normals need the interactive viewer)\n"
           "  --yaw D --pitch D      orbit angles in degrees (default 35 / 18)\n"
           "  --require-content      exit non-zero if the model falls back to the builtin placeholder\n"
           "\n"
           "  --version, -v          print version and exit\n"
           "  --help, -h             this message\n"
           "\n"
           "Determinism: snapshot mode forces AA and auto-exposure off for reproducible goldens.\n"
           "Cross-GPU byte equality is not guaranteed; pack CI should pin lavapipe + a tolerance.\n";
}

namespace {
// Parse "WxH" into w/h; returns false on any malformed input.
bool parseSize(const char* s, int& w, int& h) {
    int pw = 0, ph = 0;
    if (std::sscanf(s, "%dx%d", &pw, &ph) != 2)
        return false;
    if (pw <= 0 || ph <= 0 || pw > 16384 || ph > 16384)
        return false;
    w = pw;
    h = ph;
    return true;
}

bool needValue(const std::vector<std::string>& args, size_t& i, const char* flag, std::string& out,
               ViewerParseResult& r) {
    if (i + 1 >= args.size()) {
        r.ok = false;
        r.error = std::string("missing value for ") + flag;
        return false;
    }
    out = args[++i];
    return true;
}
} // namespace

ViewerParseResult parseViewerOptions(const std::vector<std::string>& args) {
    ViewerParseResult r;
    ViewerOptions& o = r.options;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            o.showHelp = true;
            return r;
        }
        if (a == "--version" || a == "-v") {
            o.showVersion = true;
            return r;
        }
        if (a == "--entity") {
            if (!needValue(args, i, "--entity", o.entityId, r))
                return r;
        } else if (a == "--assets") {
            if (!needValue(args, i, "--assets", o.assetsRoot, r))
                return r;
        } else if (a == "--snapshot") {
            if (!needValue(args, i, "--snapshot", o.snapshotPath, r))
                return r;
        } else if (a == "--size") {
            std::string v;
            if (!needValue(args, i, "--size", v, r))
                return r;
            if (!parseSize(v.c_str(), o.width, o.height)) {
                r.ok = false;
                r.error = "invalid --size (expected WxH, e.g. 1280x720): " + v;
                return r;
            }
        } else if (a == "--frames") {
            std::string v;
            if (!needValue(args, i, "--frames", v, r))
                return r;
            o.frames = std::atoi(v.c_str());
            if (o.frames < 1 || o.frames > 100000) {
                r.ok = false;
                r.error = "invalid --frames (expected 1..100000): " + v;
                return r;
            }
        } else if (a == "--damaged") {
            o.damaged = true;
        } else if (a == "--require-content") {
            o.requireContent = true;
        } else if (a == "--view") {
            std::string v;
            if (!needValue(args, i, "--view", v, r))
                return r;
            if (v == "shaded")
                o.view = PreviewDebugView::Shaded;
            else if (v == "facecolor")
                o.view = PreviewDebugView::FaceColor;
            else if (v == "wireframe")
                o.view = PreviewDebugView::Wireframe;
            else if (v == "normals")
                o.view = PreviewDebugView::Normals;
            else {
                r.ok = false;
                r.error = "invalid --view (shaded|facecolor|wireframe|normals): " + v;
                return r;
            }
        } else if (a == "--yaw") {
            std::string v;
            if (!needValue(args, i, "--yaw", v, r))
                return r;
            o.yawDeg = static_cast<float>(std::atof(v.c_str()));
        } else if (a == "--pitch") {
            std::string v;
            if (!needValue(args, i, "--pitch", v, r))
                return r;
            o.pitchDeg = static_cast<float>(std::atof(v.c_str()));
        } else if (!a.empty() && a[0] == '-') {
            r.ok = false;
            r.error = "unknown option: " + a;
            return r;
        } else {
            // Positional: a bare .glb path. A second positional is an error.
            if (!o.glbPath.empty()) {
                r.ok = false;
                r.error = "unexpected extra argument: " + a;
                return r;
            }
            o.glbPath = a;
        }
    }

    if (!o.glbPath.empty() && !o.entityId.empty()) {
        r.ok = false;
        r.error = "specify a .glb path OR --entity, not both";
    }
    return r;
}

} // namespace fl
