// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAsyncFilesystem.h"
#include "IAudio.h"
#include "ICursor.h"
#include "IDisplay.h"
#include "IFilesystem.h"
#include "IInput.h"
#include "IJoystick.h"
#include "ILogger.h"
#include "IRenderer.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct MockAudio : public IAudio {
    int uploadCount = 0;
    int createCount = 0;
    int playCount = 0;
    int stopCount = 0;
    AudioBufferId nextBufferId = 1;
    AudioSourceId nextSourceId = 1;

    bool init() override {
        return true;
    }
    void shutdown() override {}
    const char* getLastError() const override {
        return nullptr;
    }

    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        ++uploadCount;
        return nextBufferId++;
    }
    void freeBuffer(AudioBufferId) override {}

    AudioSourceId createSource() override {
        ++createCount;
        return nextSourceId++;
    }
    void destroySource(AudioSourceId) override {}

    void play(AudioSourceId, AudioBufferId) override {
        ++playCount;
    }
    void stop(AudioSourceId) override {
        ++stopCount;
    }
    void pause(AudioSourceId) override {}
    void resume(AudioSourceId) override {}
    bool isPlaying(AudioSourceId) const override {
        return false;
    }

    void setLooping(AudioSourceId, bool) override {}
    void setPitch(AudioSourceId, float) override {}
    void setGain(AudioSourceId, float) override {}
    void setPosition(AudioSourceId, float, float, float) override {}
    void setVelocity(AudioSourceId, float, float, float) override {}
    void setSourceRelative(AudioSourceId, bool) override {}
    void setReferenceDistance(AudioSourceId, float) override {}
    void setMaxDistance(AudioSourceId, float) override {}
    void setRolloffFactor(AudioSourceId, float) override {}
    void setListenerTransform(const float[3], const float[3], const float[3]) override {}
    void setListenerVelocity(const float[3]) override {}
};

struct MockInput : public IInput {
    std::set<Key> justPressed;
    std::set<Key> held;
    int gamepadCount = 0;
    std::map<std::pair<int, GamepadAxis>, float> axisValues;
    std::set<std::pair<int, GamepadButton>> gpJustPressed;

    bool isKeyDown(Key k) const override {
        return held.count(k) > 0;
    }
    bool isKeyJustPressed(Key k) const override {
        return justPressed.count(k) > 0;
    }
    const char* getKeyName(Key) const override {
        return "Unknown";
    }

    void getMousePosition(int& x, int& y) const override {
        x = y = 0;
    }
    void getMouseDelta(int& dx, int& dy) const override {
        dx = dy = 0;
    }
    void setMouseCapture(bool) override {}
    int getMouseScroll() const override {
        return 0;
    }
    bool isMouseButtonDown(MouseButton) const override {
        return false;
    }

    void startTextInput(ITextInputHandler*) override {}
    void stopTextInput() override {}
    void flush() override {}

    int getGamepadCount() const override {
        return gamepadCount;
    }
    bool isGamepadButtonDown(int, GamepadButton) const override {
        return false;
    }
    bool isGamepadButtonJustPressed(int id, GamepadButton b) const override {
        return gpJustPressed.count({id, b}) > 0;
    }
    float getGamepadAxis(int id, GamepadAxis ax) const override {
        auto it = axisValues.find({id, ax});
        return it != axisValues.end() ? it->second : 0.0f;
    }
    void rumble(int, float, float, uint32_t) override {}
    void rumbleTriggers(int, float, float, uint32_t) override {}
    bool supportsRumble(int) const override {
        return false;
    }
    bool supportsTriggerRumble(int) const override {
        return false;
    }
    void stopRumble(int) override {}
};

struct MockLogger : public ILogger {
    struct Entry {
        LogLevel level;
        std::string message;
    };
    std::vector<Entry> entries;

    void log(LogLevel level, const char*, int, const char* message) override {
        entries.push_back({level, message});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    bool hasMessage(LogLevel level, const std::string& substr) const {
        for (auto& e : entries)
            if (e.level == level && e.message.find(substr) != std::string::npos)
                return true;
        return false;
    }
};

struct MockFilesystem : public IFilesystem {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, std::vector<Entry>> dirs;

    bool createDirectoryResult = true;
    bool failWriteOpen = false;
    bool renameResult = true;

    struct RenameCall {
        std::string from;
        std::string to;
    };
    std::vector<RenameCall> renameCalls;

    void addFile(const std::string& path, const std::string& content) {
        files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }

    int openFile(PathDomain, const char* path, bool write) override {
        if (write) {
            if (failWriteOpen)
                return -1;
            files[path] = {};
            writeHandles[nextHandle] = path;
            return nextHandle++;
        }
        auto it = files.find(path);
        if (it == files.end())
            return -1;
        readHandles[nextHandle] = path;
        return nextHandle++;
    }

    void closeFile(int handle) override {
        readHandles.erase(handle);
        writeHandles.erase(handle);
    }

    std::size_t readFile(int handle, void* buffer, std::size_t size) override {
        auto hit = readHandles.find(handle);
        if (hit == readHandles.end())
            return 0;
        auto& data = files[hit->second];
        std::size_t n = std::min(size, data.size());
        std::memcpy(buffer, data.data(), n);
        return n;
    }

    std::size_t writeFile(int handle, const void* data, std::size_t size) override {
        auto hit = writeHandles.find(handle);
        if (hit == writeHandles.end())
            return 0;
        auto& buf = files[hit->second];
        const auto* bytes = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + size);
        return size;
    }

    bool seek(int, std::size_t, SeekOrigin) override {
        return false;
    }

    std::size_t getFileSize(int handle) const override {
        auto hit = readHandles.find(handle);
        if (hit == readHandles.end())
            return 0;
        auto fit = files.find(hit->second);
        return (fit != files.end()) ? fit->second.size() : 0;
    }

    bool fileExists(PathDomain, const char* path) const override {
        return files.find(path) != files.end();
    }

    bool createDirectory(PathDomain, const char*) override {
        return createDirectoryResult;
    }

    bool renameFile(PathDomain, const char* from, const char* to) override {
        renameCalls.push_back({from, to});
        if (renameResult && files.count(from)) {
            files[to] = std::move(files[from]);
            files.erase(from);
        }
        return renameResult;
    }

    std::vector<Entry> scanDirectory(PathDomain, const char* path) const override {
        auto it = dirs.find(path);
        if (it == dirs.end())
            return {};
        return it->second;
    }

  private:
    int nextHandle = 1;
    std::map<int, std::string> readHandles;
    std::map<int, std::string> writeHandles;
};

struct MockAsyncFilesystem : public IAsyncFilesystem {
    std::map<std::string, std::vector<uint8_t>> files;

    struct PendingRead {
        AsyncReadId id;
        PathDomain domain;
        std::string path;
        bool cancelled{false};
    };
    std::vector<PendingRead> pending;

    AsyncReadId nextId{1};
    IAsyncFilesystemHandler* handler{nullptr};
    bool initialized{false};
    std::string lastErrorBuf;

    void addFile(const std::string& path, const std::vector<uint8_t>& data) {
        files[path] = data;
    }
    void addFile(const std::string& path, const std::string& content) {
        files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }

    bool init() override {
        initialized = true;
        return true;
    }
    void shutdown() override {
        for (auto& p : pending)
            if (handler)
                handler->onReadComplete(p.id, AsyncReadStatus::Cancelled, nullptr, 0, nullptr);
        pending.clear();
        initialized = false;
    }
    void setEventHandler(IAsyncFilesystemHandler* h) override {
        handler = h;
    }

    AsyncReadId readFileAsync(PathDomain domain, const char* path) override {
        if (!initialized || !path)
            return 0;
        AsyncReadId id = nextId++;
        pending.push_back({id, domain, std::string(path), false});
        return id;
    }
    void cancelRead(AsyncReadId id) override {
        for (auto& p : pending)
            if (p.id == id) {
                p.cancelled = true;
                return;
            }
    }
    void service() override {
        std::vector<PendingRead> batch;
        std::swap(batch, pending);
        for (auto& p : batch) {
            if (p.cancelled) {
                if (handler)
                    handler->onReadComplete(p.id, AsyncReadStatus::Cancelled, nullptr, 0, nullptr);
                continue;
            }
            auto it = files.find(p.path);
            if (it == files.end()) {
                lastErrorBuf = "file not found: " + p.path;
                if (handler)
                    handler->onReadComplete(p.id, AsyncReadStatus::Error, nullptr, 0, lastErrorBuf.c_str());
            } else {
                if (handler)
                    handler->onReadComplete(p.id, AsyncReadStatus::Success, it->second.data(), it->second.size(),
                                            nullptr);
            }
        }
    }
    const char* getLastError() const override {
        return lastErrorBuf.empty() ? nullptr : lastErrorBuf.c_str();
    }
};

struct MockCursor : public ICursor {
    CursorShape lastShape{CursorShape::Arrow};
    bool customCursorSet{false};

    void setCursor(CursorShape shape) override {
        lastShape = shape;
    }
    void setCustomCursor(const void*, int, int, int, int) override {
        customCursorSet = true;
    }
    const char* getLastError() const override {
        return nullptr;
    }
};

struct MockDisplay : public IDisplay {
    int monitorCount = 1;
    std::vector<DisplayMode> modes;
    float mockRefreshRate = 60.0f;

    int getMonitorCount() const override {
        return monitorCount;
    }
    const char* getMonitorName(int id) const override {
        return (id >= 0 && id < monitorCount) ? "Mock Monitor" : nullptr;
    }
    std::vector<DisplayMode> listModes(int) const override {
        return modes;
    }
    float getRefreshRate(int id) const override {
        return (id >= 0 && id < monitorCount) ? mockRefreshRate : 0.0f;
    }
    const char* getLastError() const override {
        return nullptr;
    }
};

struct MockJoystick : public IJoystick {
    int count = 0;

    int getJoystickCount() const override {
        return count;
    }
    const char* getJoystickName(int) const override {
        return "MockJoystick";
    }
    const char* getJoystickGuid(int) const override {
        return "00000000000000000000000000000000";
    }
    int getAxisCount(int) const override {
        return 0;
    }
    float getAxisValue(int, int) const override {
        return 0.0f;
    }
    int getHatCount(int) const override {
        return 0;
    }
    HatPosition getHatPosition(int, int) const override {
        return HatPosition::Centered;
    }
    int getButtonCount(int) const override {
        return 0;
    }
    bool isButtonDown(int, int) const override {
        return false;
    }
    bool isButtonJustPressed(int, int) const override {
        return false;
    }
    void setEventHandler(IJoystickEventHandler*) override {}
    void flush() override {}
    const char* getLastError() const override {
        return nullptr;
    }
};

struct MockRenderer : public IRenderer {
    int initCount{0};
    int shutdownCount{0};
    int beginFrameCount{0};
    int endFrameCount{0};
    int resizeCount{0};
    int lastResizeW{0};
    int lastResizeH{0};
    bool initResult{true};
    std::string lastErrorBuf;

    // Resource tracking
    uint32_t nextMeshId{1};
    uint32_t nextTextureId{1};
    uint32_t nextMaterialId{1};
    int createMeshCount{0};
    int createTextureCount{0};
    int createMaterialCount{0};
    int destroyMeshCount{0};
    int destroyTextureCount{0};
    int destroyMaterialCount{0};
    int setSceneCount{0};
    FrameScene lastScene{};

    bool init(IWindow*) override {
        ++initCount;
        return initResult;
    }
    void onResize(int w, int h) override {
        ++resizeCount;
        lastResizeW = w;
        lastResizeH = h;
    }
    void beginFrame() override {
        ++beginFrameCount;
    }
    void endFrame() override {
        ++endFrameCount;
    }
    void shutdown() override {
        ++shutdownCount;
    }
    const char* getLastError() const override {
        return lastErrorBuf.empty() ? nullptr : lastErrorBuf.c_str();
    }
    const char* gpuInfo() const override {
        return "MockGPU 1.0";
    }

    MeshHandle createMesh(const MeshUploadDesc&) override {
        ++createMeshCount;
        return MeshHandle{nextMeshId++};
    }
    TextureHandle createTexture(const TextureUploadDesc&) override {
        ++createTextureCount;
        return TextureHandle{nextTextureId++};
    }
    MaterialHandle createMaterial(const MaterialDesc&) override {
        ++createMaterialCount;
        return MaterialHandle{nextMaterialId++};
    }
    void destroyMesh(MeshHandle) override {
        ++destroyMeshCount;
    }
    void destroyTexture(TextureHandle) override {
        ++destroyTextureCount;
    }
    void destroyMaterial(MaterialHandle) override {
        ++destroyMaterialCount;
    }
    void setScene(const FrameScene& scene) override {
        ++setSceneCount;
        lastScene = scene;
    }
    void applySettings(const RendererSettings&) override {}
};
