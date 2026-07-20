// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VideoEncoderPipe (#916) — pipes raw RGBA frames from the cinematic recorder to an external ffmpeg
// process (H.264 mp4) or, as a fallback, writes a PNG sequence. ffmpeg is a record-time TOOL dependency
// only — never linked; frames are streamed to its stdin via popen. All platform #ifdefs are confined to
// the .cpp, and the PNG fallback reuses the already-compiled stb_image_write (platform-vulkan).

#include <cstdint>
#include <string>

namespace fl {

class VideoEncoderPipe {
  public:
    VideoEncoderPipe() = default;
    ~VideoEncoderPipe();

    VideoEncoderPipe(const VideoEncoderPipe&) = delete;
    VideoEncoderPipe& operator=(const VideoEncoderPipe&) = delete;

    // Open an ffmpeg subprocess encoding `width`x`height` RGBA frames at `fps` to `outPath` (mp4/H.264).
    // Returns false (and sets lastError) if ffmpeg cannot be spawned — the caller should then try a PNG
    // sequence or fail loudly. `ffmpegBin` overrides the "ffmpeg" executable name (empty = "ffmpeg").
    bool openFfmpeg(const std::string& outPath, uint32_t width, uint32_t height, int fps,
                    const std::string& ffmpegBin = {});

    // Open a PNG-sequence sink: frames are written as `<dir>/frame_000000.png`, ... (dir must exist).
    bool openPngDir(const std::string& dir, uint32_t width, uint32_t height);

    [[nodiscard]] bool isOpen() const noexcept {
        return m_pipe != nullptr || m_pngMode;
    }

    // Push one tightly-packed RGBA8 frame (width*height*4 bytes). Returns false on a write error.
    bool pushFrame(const uint8_t* rgba, uint32_t width, uint32_t height);

    // Flush + close the encoder. For ffmpeg, waits for the process and returns true only on a clean
    // (zero) exit. Safe to call more than once (the destructor also calls it). Returns false if the
    // encoder never opened or the process exited non-zero.
    bool close();

    [[nodiscard]] const char* lastError() const noexcept {
        return m_lastError.empty() ? nullptr : m_lastError.c_str();
    }
    [[nodiscard]] uint64_t framesWritten() const noexcept {
        return m_framesWritten;
    }

  private:
    void* m_pipe{nullptr}; // FILE* from popen (ffmpeg stdin); void* to keep <cstdio> out of the header
    bool m_pngMode{false};
    std::string m_pngDir;
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint64_t m_framesWritten{0};
    std::string m_lastError;
    bool m_closed{false};
};

} // namespace fl
