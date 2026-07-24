// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------------------------
// Microphone capture HAL (#531)
// ---------------------------------------------------------------------------------------------
// Separate from IAudio on purpose. IAudio is OpenAL Soft, whose capture side is the optional
// ALC_EXT_CAPTURE extension: present on some drivers, absent on others, and with no device
// enumeration worth exposing in a settings screen. Capture is also a fundamentally different
// lifecycle — it is started and stopped every time the player keys the mic, while playback devices
// stay open for the session. Folding it into IAudio would give every mock and every headless
// binary six methods they must stub for a capability they never use.
//
// Threading: the backend owns whatever thread the OS hands it; every method here is called from
// the MAIN thread and read() drains an internal, lock-protected ring. Callers never see a
// capture-thread callback.
//
// Failure is always a soft failure: no device, no permission, or no backend at all leaves the
// capture object usable and read() returning 0 samples, so the voice pipeline degrades to
// "listen-only" rather than the client refusing to run.
// ---------------------------------------------------------------------------------------------
class IAudioCapture {
  public:
    virtual ~IAudioCapture() = default;

    // Opens the capture device at the requested format. `deviceName` empty = system default; an
    // unknown name falls back to the default rather than failing. Returns false if no device could
    // be opened (the object stays valid and inert).
    virtual bool init(int sampleRate, int channels, const std::string& deviceName = {}) = 0;
    virtual void shutdown() = 0;

    // Human-readable description of the last failure, or nullptr. Valid until the next call.
    virtual const char* getLastError() const = 0;

    // Begin/stop filling the ring. Stopping DISCARDS buffered audio: whatever was mid-flight when
    // the player released the key is by definition not part of the transmission.
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isCapturing() const = 0;

    // Drain up to maxSamples interleaved int16 samples. Returns the count actually written; 0 when
    // not capturing, not yet enough data, or unavailable.
    virtual std::size_t read(int16_t* out, std::size_t maxSamples) = 0;

    // Samples currently buffered and readable.
    virtual std::size_t available() const = 0;

    // Enumerate capture devices for the settings UI. The default device is NOT included as a named
    // entry: the empty string means "default", and pinning the current default by name would
    // silently keep using a headset the player has since unplugged.
    virtual std::vector<std::string> listDevices() const = 0;

    // The device actually opened (may differ from the request when it fell back to the default).
    virtual std::string currentDevice() const = 0;
};

} // namespace fl
