// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Factory.h"
#include "SDL3Display.h"
#include "SDL3Input.h"
#include "SDL3Window.h"

SDL3WindowInput createSDL3WindowInput() {
    auto input = std::make_unique<SDL3Input>();
    auto window = std::make_unique<SDL3Window>();
    window->setInputSink(input.get());
    return SDL3WindowInput{std::move(input), std::move(window)};
}

std::unique_ptr<IDisplay> createSDL3Display() {
    return std::make_unique<SDL3Display>();
}
