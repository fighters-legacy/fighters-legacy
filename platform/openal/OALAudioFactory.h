// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header. Include this instead of OALAudio.h so consumers are
// never exposed to OpenAL headers.
#include "IAudio.h"
#include <memory>

std::unique_ptr<IAudio> createOALAudio();
