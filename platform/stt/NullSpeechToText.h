// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ISpeechToText.h"

// The no-transcriber transcriber (#935).
//
// What a build without FL_ENABLE_WHISPER gets, and what an enabled build falls back to when the
// model file is missing or will not load. available() is false and submit() refuses at the call, so
// the voice tier degrades to the radio menu by ASKING rather than by a caller discovering that
// transcripts never arrive.

namespace fl {

class NullSpeechToText final : public ISpeechToText {
  public:
    bool init(ILogger&) override {
        return true;
    }
    void shutdown() override {}
    void setHandler(ISpeechToTextHandler*) override {}
    SttRequestId submit(const int16_t*, std::size_t, int) override {
        return 0;
    }
    void cancel(SttRequestId) override {}
    void service() override {}
    [[nodiscard]] bool available() const override {
        return false;
    }
    [[nodiscard]] const char* getLastError() const override {
        return "speech-to-text is not available in this build";
    }
};

} // namespace fl
