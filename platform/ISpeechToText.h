// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Speech-to-text HAL (#935).
//
// The deterministic voice-command tier: push-to-talk audio becomes a transcript, and
// fl::ai::matchWingmanPhrase turns that transcript into one of the six scripted commands. No LLM
// anywhere on this path — it is CPU-viable and independent of the #769 GPU decision, so every server
// can offer "talk to your wingman" with the scripted fallback intact.
//
// A HAL seam, not a direct whisper.cpp call, for the reason IHttpClient is one: the engine takes it
// by injection and cmake/layering.cmake forbids any engine-* target from reaching the backend. It
// also means the whole path is testable against a canned transcriber with no model file on disk.
//
// THREADING mirrors IAsyncFilesystem and IWorldAiProvider, because transcription takes hundreds of
// milliseconds and must not block a frame:
//
//   submit()   hands over a finished utterance and returns immediately
//   service()  drains completions and fires the handler ON THE CALLING THREAD
//   shutdown() cancels and joins
//
// Audio arrives as one COMPLETE utterance rather than as a stream. Push-to-talk gives a natural
// boundary — the player released the key — and a streaming interface would buy nothing but the
// obligation to decide when someone stopped talking.

namespace fl {

class ILogger;

using SttRequestId = uint32_t; // 0 = the request could not be enqueued

enum class SttStatus : uint8_t {
    Success,
    Error,     // transcription failed; `error` says why
    Cancelled, // shutdown() or cancel() reached it first
};

class ISpeechToTextHandler {
  public:
    virtual ~ISpeechToTextHandler() = default;

    // `text` is valid only for the duration of the call. It is UNTRUSTED in the same sense player
    // chat is: it is whatever a microphone in a room produced, so it goes through the phrase matcher
    // rather than anywhere near a command string.
    virtual void onTranscript(SttRequestId id, SttStatus status, const std::string& text, const std::string& error) = 0;
};

class ISpeechToText {
  public:
    virtual ~ISpeechToText() = default;

    // Load the model and start the worker. False on failure — a missing or unreadable model file is
    // the common one — and the object stays valid and inert, so the caller degrades to the radio
    // menu rather than handling a null.
    virtual bool init(ILogger& logger) = 0;
    virtual void shutdown() = 0;

    virtual void setHandler(ISpeechToTextHandler* handler) = 0;

    // One complete utterance: interleaved mono 16-bit PCM at `sampleRate`. Returns 0 when the
    // request could not be enqueued (not initialised, shutting down, empty audio, or the queue is
    // full — a queue bound matters here because a stuck key would otherwise enqueue forever).
    virtual SttRequestId submit(const int16_t* samples, std::size_t sampleCount, int sampleRate) = 0;

    virtual void cancel(SttRequestId id) = 0;

    // Drain completions and fire the handler on this thread. Call once per frame.
    virtual void service() = 0;

    // True when a model is loaded and transcription can actually happen. False after a failed init,
    // and always false for the null backend — the caller asks this rather than discovering.
    [[nodiscard]] virtual bool available() const = 0;

    [[nodiscard]] virtual const char* getLastError() const = 0;
};

// The backend factory. Returns a working transcriber when the build has one and `modelPath` loads,
// and otherwise a null implementation whose available() is false — one degradation path, the same
// shape createHttpClient and loadWorldAiProvider use.
//
// Built only when FL_ENABLE_WHISPER is ON; a lean build links cleanly and gets the null backend.
[[nodiscard]] std::unique_ptr<ISpeechToText> createSpeechToText(const std::string& modelPath, ILogger& logger);

} // namespace fl
