// SPDX-License-Identifier: GPL-3.0-or-later
#include "VideoEncoderPipe.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#else
#include <csignal>
#endif

// stb_image_write is compiled once in platform/vulkan/VkResources.cpp (STB_IMAGE_WRITE_IMPLEMENTATION);
// declare the single entry point we use for the PNG-sequence fallback rather than re-including the impl.
extern "C" int stbi_write_png(const char* filename, int w, int h, int comp, const void* data, int stride_bytes);

namespace fl {

namespace {

// Quote a path for a shell command line (popen runs via /bin/sh or cmd.exe). Wrap in double quotes and
// escape embedded quotes; good enough for the record-time output paths we control.
std::string shellQuote(const std::string& s) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '$' || c == '`')
            out += '\\';
        out += c;
    }
    out += "\"";
    return out;
#endif
}

} // namespace

VideoEncoderPipe::~VideoEncoderPipe() {
    close();
}

bool VideoEncoderPipe::openFfmpeg(const std::string& outPath, uint32_t width, uint32_t height, int fps,
                                  const std::string& ffmpegBin) {
    if (width == 0 || height == 0 || fps <= 0) {
        m_lastError = "openFfmpeg: invalid dimensions or fps";
        return false;
    }
    m_width = width;
    m_height = height;

#if !defined(_WIN32)
    // Ignore SIGPIPE process-wide: if ffmpeg exits early (missing codec, disk full), a write to the
    // closed pipe must return an error we handle, not kill the recorder with signal 13.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Read raw RGBA frames from stdin, encode H.264 to an mp4. -pix_fmt yuv420p keeps the output widely
    // playable; -crf 20 is a good visual/size tradeoff for demo footage.
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%ux%u", width, height);
    const std::string bin = ffmpegBin.empty() ? std::string("ffmpeg") : ffmpegBin;
    std::string cmd = bin + " -y -loglevel error -f rawvideo -pixel_format rgba -video_size " + sz + " -framerate " +
                      std::to_string(fps) + " -i - -an";
    // H.264 in yuv420p needs EVEN dimensions, and a capture surface is not always even: a 960x540
    // window under a 125%-scaling compositor has a 1200x675 drawable, and libx264 refuses it outright
    // ("height not divisible by 2") -- the whole recording is lost to one row (#1347). Crop the odd
    // edge instead. Only added when it is needed, so an even-sized recording runs no filter at all.
    if ((width % 2u) != 0u || (height % 2u) != 0u)
        cmd += " -vf \"crop=trunc(iw/2)*2:trunc(ih/2)*2\"";
    cmd += " -c:v libx264 -crf 20 -pix_fmt yuv420p " + shellQuote(outPath);

    m_pipe = ::popen(cmd.c_str(), "w");
    if (!m_pipe) {
        m_lastError = "openFfmpeg: popen failed (is ffmpeg installed and on PATH?)";
        return false;
    }
    m_closed = false;
    return true;
}

bool VideoEncoderPipe::openPngDir(const std::string& dir, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        m_lastError = "openPngDir: invalid dimensions";
        return false;
    }
    m_pngMode = true;
    m_pngDir = dir;
    m_width = width;
    m_height = height;
    m_closed = false;
    return true;
}

bool VideoEncoderPipe::pushFrame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width != m_width || height != m_height) {
        m_lastError = "pushFrame: null pixels or dimension mismatch";
        return false;
    }
    if (m_pngMode) {
        char name[512];
        std::snprintf(name, sizeof(name), "%s/frame_%06llu.png", m_pngDir.c_str(),
                      static_cast<unsigned long long>(m_framesWritten));
        if (stbi_write_png(name, static_cast<int>(width), static_cast<int>(height), 4, rgba,
                           static_cast<int>(width) * 4) == 0) {
            m_lastError = std::string("pushFrame: stbi_write_png failed for ") + name;
            return false;
        }
        ++m_framesWritten;
        return true;
    }
    if (!m_pipe) {
        m_lastError = "pushFrame: encoder not open";
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
    if (std::fwrite(rgba, 1, bytes, static_cast<std::FILE*>(m_pipe)) != bytes) {
        m_lastError = "pushFrame: short write to ffmpeg (encoder may have exited)";
        return false;
    }
    ++m_framesWritten;
    return true;
}

bool VideoEncoderPipe::close() {
    if (m_closed)
        return m_lastError.empty();
    m_closed = true;
    if (m_pngMode) {
        m_pngMode = false;
        return true;
    }
    if (!m_pipe)
        return false;
    const int rc = ::pclose(static_cast<std::FILE*>(m_pipe));
    m_pipe = nullptr;
    if (rc != 0) {
        m_lastError = "ffmpeg exited with a non-zero status (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

} // namespace fl
