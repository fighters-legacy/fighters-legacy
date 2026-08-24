// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/BuiltinTextures.h"

namespace fl {

namespace {

// True on a seam pixel: a grid of grooves every 16 px (one px wide). The grooves darken the base
// color, tilt the normal, and roughen the ORM, so all three maps share one visible feature.
bool onSeam(int x, int y) {
    return (x % 16 == 0) || (y % 16 == 0);
}

} // namespace

BuiltinRgbaTexture builtinBaseColorTexture() {
    BuiltinRgbaTexture t = makeBuiltinRgba();
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            // A faint checker over a mid grey, darkened along the seams.
            const bool check = ((x / 16) + (y / 16)) % 2 == 0;
            uint8_t base = check ? 150 : 130;
            if (onSeam(x, y))
                base = static_cast<uint8_t>(base * 6 / 10);
            putPixel(t, x, y, base, static_cast<uint8_t>(base * 105 / 100), static_cast<uint8_t>(base * 95 / 100), 255);
        }
    return t;
}

BuiltinRgbaTexture builtinNormalTexture() {
    BuiltinRgbaTexture t = makeBuiltinRgba();
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            // Flat (128,128,255) everywhere except a slight tilt at the seams so lighting reacts.
            uint8_t nx = 128, ny = 128;
            if (x % 16 == 0)
                nx = 100; // groove tilts along -X
            if (y % 16 == 0)
                ny = 100;
            putPixel(t, x, y, nx, ny, 255, 255);
        }
    return t;
}

BuiltinRgbaTexture builtinOrmTexture() {
    BuiltinRgbaTexture t = makeBuiltinRgba();
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            // R=occlusion (full), G=roughness (~0.7, rougher in the grooves), B=metallic (0).
            const uint8_t rough = onSeam(x, y) ? 220 : 180;
            putPixel(t, x, y, 255, rough, 0, 255);
        }
    return t;
}

} // namespace fl
