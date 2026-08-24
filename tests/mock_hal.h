// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAsyncFilesystem.h"
#include "IAudio.h"
#include "ICursor.h"
#include "IDisplay.h"
#include "IFilesystem.h"
#include "IFilesystemWatcher.h"
#include "IInput.h"
#include "IJoystick.h"
#include "ILogger.h"
#include "IRenderer.h"
#include "IWindow.h"
#include "NullAudio.h"

#include "mock_log.h"

#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Shared HAL test doubles live here — define new ones here rather than re-declaring per test file.
// Naming convention: Null* = no-op base, Tracking* = records calls, Fake* = limited real behaviour,
// Mock* = configurable. Two interfaces are kept in dedicated headers to avoid forcing their deps on
// every HAL-only test: INetwork doubles in mock_network.h, IContentPack doubles in mock_content.h,
// ILogger doubles in mock_log.h, which this header includes — so MockLogger and NullLogger are
// available to anything that includes mock_hal.h, as they always were.

namespace fl {

struct MockAudio : public fl::NullAudio {
    int uploadCount = 0;
    int createCount = 0;
    int playCount = 0;
    int stopCount = 0;
    AudioBufferId nextBufferId = 1;
    AudioSourceId nextSourceId = 1;

    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        ++uploadCount;
        return nextBufferId++;
    }

    AudioBufferId allocStreamBuffer() override {
        return nextBufferId++;
    }

    AudioSourceId createSource() override {
        ++createCount;
        return nextSourceId++;
    }

    void play(AudioSourceId, AudioBufferId) override {
        ++playCount;
    }
    void stop(AudioSourceId) override {
        ++stopCount;
    }
};

struct MockInput : public IInput {
    std::set<Key> justPressed;
    std::set<Key> held;
    int gamepadCount = 0;
    std::map<std::pair<int, GamepadAxis>, float> axisValues;
    std::set<std::pair<int, GamepadButton>> gpDown;
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

    int mouseX = 0;
    int mouseY = 0;
    void getMousePosition(int& x, int& y) const override {
        x = mouseX;
        y = mouseY;
    }
    void getMouseDelta(int& dx, int& dy) const override {
        dx = dy = 0;
    }
    void setMouseCapture(bool) override {}
    int getMouseScroll() const override {
        return 0;
    }
    std::set<MouseButton> mouseDown;
    std::set<MouseButton> mouseJustPressed;
    bool isMouseButtonDown(MouseButton b) const override {
        return mouseDown.count(b) > 0;
    }
    bool isMouseButtonJustPressed(MouseButton b) const override {
        return mouseJustPressed.count(b) > 0;
    }

    void startTextInput(ITextInputHandler*) override {}
    void stopTextInput() override {}
    void flush() override {}

    int getGamepadCount() const override {
        return gamepadCount;
    }
    bool isGamepadButtonDown(int id, GamepadButton b) const override {
        return gpDown.count({id, b}) > 0;
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

// The recorder now lives in mock_log.h so suites that link no HAL can use it too; the name stays
// for this header's ~30 existing users.
using MockLogger = RecordingLogger;

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
    void addDir(const std::string& path) {
        if (dirs.find(path) == dirs.end())
            dirs[path] = {};
    }
    void addDirEntry(const std::string& parentDir, const std::string& name, bool isDirectory) {
        dirs[parentDir].push_back({name, isDirectory});
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
        if (n > 0) // data.data() is nullptr for an empty file; memcpy(_, nullptr, 0) is UB
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
        // Per-read handler (#1083): routed by id, exactly as the real backend does, so a test can drive
        // two consumers at once and see that neither takes the other's completions.
        IAsyncFilesystemHandler* handler{nullptr};
    };
    std::vector<PendingRead> pending;

    AsyncReadId nextId{1};
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
            if (p.handler)
                p.handler->onReadComplete(p.id, AsyncReadStatus::Cancelled, nullptr, 0, nullptr);
        pending.clear();
        initialized = false;
    }

    AsyncReadId readFileAsync(PathDomain domain, const char* path, IAsyncFilesystemHandler* h) override {
        if (!initialized || !path || h == nullptr)
            return 0;
        AsyncReadId id = nextId++;
        pending.push_back({id, domain, std::string(path), false, h});
        return id;
    }
    void cancelRead(AsyncReadId id) override {
        for (auto& p : pending)
            if (p.id == id) {
                p.cancelled = true;
                return;
            }
    }
    void cancelReadsFor(IAsyncFilesystemHandler* h) override {
        for (auto& p : pending)
            if (p.handler == h) {
                p.cancelled = true;
                p.handler = nullptr; // and deliver nothing: the owner is going away
            }
    }
    void service() override {
        std::vector<PendingRead> batch;
        std::swap(batch, pending);
        for (auto& p : batch) {
            if (!p.handler)
                continue; // forgotten by cancelReadsFor
            if (p.cancelled) {
                p.handler->onReadComplete(p.id, AsyncReadStatus::Cancelled, nullptr, 0, nullptr);
                continue;
            }
            auto it = files.find(p.path);
            if (it == files.end()) {
                lastErrorBuf = "file not found: " + p.path;
                p.handler->onReadComplete(p.id, AsyncReadStatus::Error, nullptr, 0, lastErrorBuf.c_str());
            } else {
                p.handler->onReadComplete(p.id, AsyncReadStatus::Success, it->second.data(), it->second.size(),
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
    // Per-device state (#1061): a test that needs two distinguishable sticks, buttons or a POV hat
    // pushes `devices`. When it is empty the older convenience surface below applies, so tests written
    // against the single-device model keep working unchanged.
    struct Device {
        std::string guid{"00000000000000000000000000000000"};
        std::string name{"MockJoystick"};
        std::vector<float> axes;
        std::vector<bool> buttons;
        std::vector<bool> justPressed;
        std::vector<HatPosition> hats;
    };
    std::vector<Device> devices;

    // Convenience surface: `count` identical devices with `axisCount` axes each, values read from
    // axisValues[{device, axis}].
    int count = 0;
    int axisCount = 0;
    std::map<std::pair<int, int>, float> axisValues;

    Device& addDevice(std::string guid, std::string name = "MockJoystick") {
        Device d;
        d.guid = std::move(guid);
        d.name = std::move(name);
        devices.push_back(std::move(d));
        return devices.back();
    }

    const Device* at(int j) const {
        if (j < 0 || j >= static_cast<int>(devices.size()))
            return nullptr;
        return &devices[static_cast<size_t>(j)];
    }

    int getJoystickCount() const override {
        return devices.empty() ? count : static_cast<int>(devices.size());
    }
    const char* getJoystickName(int j) const override {
        const Device* d = at(j);
        return d ? d->name.c_str() : "MockJoystick";
    }
    const char* getJoystickGuid(int j) const override {
        const Device* d = at(j);
        return d ? d->guid.c_str() : "00000000000000000000000000000000";
    }
    int getAxisCount(int j) const override {
        const Device* d = at(j);
        return d ? static_cast<int>(d->axes.size()) : axisCount;
    }
    float getAxisValue(int j, int a) const override {
        if (const Device* d = at(j))
            return (a >= 0 && a < static_cast<int>(d->axes.size())) ? d->axes[static_cast<size_t>(a)] : 0.0f;
        auto it = axisValues.find({j, a});
        return it != axisValues.end() ? it->second : 0.0f;
    }
    int getHatCount(int j) const override {
        const Device* d = at(j);
        return d ? static_cast<int>(d->hats.size()) : 0;
    }
    HatPosition getHatPosition(int j, int h) const override {
        const Device* d = at(j);
        if (!d || h < 0 || h >= static_cast<int>(d->hats.size()))
            return HatPosition::Centered;
        return d->hats[static_cast<size_t>(h)];
    }
    int getButtonCount(int j) const override {
        const Device* d = at(j);
        return d ? static_cast<int>(d->buttons.size()) : 0;
    }
    bool isButtonDown(int j, int btn) const override {
        const Device* d = at(j);
        if (!d || btn < 0 || btn >= static_cast<int>(d->buttons.size()))
            return false;
        return d->buttons[static_cast<size_t>(btn)];
    }
    bool isButtonJustPressed(int j, int btn) const override {
        const Device* d = at(j);
        if (!d || btn < 0 || btn >= static_cast<int>(d->justPressed.size()))
            return false;
        return d->justPressed[static_cast<size_t>(btn)];
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
    int createTextureArrayCount{0};             // #446 biome array uploads
    uint32_t lastTextureArrayLayers{0};         // layerCount of the last array upload
    int setTerrainBiomeTexturesCount{0};        // #446 set-2 biome binding
    std::vector<MaterialDesc> createdMaterials; // every MaterialDesc passed to createMaterial (#867)
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
    TextureHandle createTextureArray(const TextureUploadDesc& desc) override {
        ++createTextureArrayCount;
        lastTextureArrayLayers = desc.rawLayers;
        return TextureHandle{nextTextureId++};
    }
    void setTerrainBiomeTextures(TextureHandle, TextureHandle, uint32_t layerCount) override {
        ++setTerrainBiomeTexturesCount;
        lastTextureArrayLayers = layerCount;
    }
    MaterialHandle createMaterial(const MaterialDesc& desc) override {
        ++createMaterialCount;
        createdMaterials.push_back(desc); // so tests can inspect texture bindings (#867)
        return MaterialHandle{nextMaterialId++};
    }
    MaterialHandle getMeshMaterial(MeshHandle) const override {
        return meshMaterialResult; // default invalid; a test can script a valid material
    }
    MaterialHandle meshMaterialResult{}; // returned by getMeshMaterial (#836 PreviewScene tests)

    // Scripted mesh bounds (#836): when meshBoundsAvailable, getMeshBounds fills these and returns
    // true; otherwise false (the non-supporting-backend default), so framing falls back.
    bool meshBoundsAvailable{false};
    glm::vec3 meshBoundsMin{0.0f};
    glm::vec3 meshBoundsMax{0.0f};
    bool getMeshBounds(MeshHandle, glm::vec3& outMin, glm::vec3& outMax) const override {
        if (!meshBoundsAvailable)
            return false;
        outMin = meshBoundsMin;
        outMax = meshBoundsMax;
        return true;
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
    RendererSettings lastApplied{};
    void applySettings(const RendererSettings& s) override {
        lastApplied = s;
    }
    FrameStats getFrameStats() const override {
        return {};
    }
    void setOverlayLines(std::span<const std::string_view>) override {}
    void submitOverlayElements(std::span<const HudElement>) override {}
    void setConsoleElements(std::span<const HudElement>) override {}
};

// The IFilesystemWatcher double (#1276). Records both directions: two suites had near-identical
// copies and only one of them recorded unwatch, so a leak of watches was invisible in the other.
struct MockFilesystemWatcher : public IFilesystemWatcher {
    struct WatchCall {
        std::string path;
        bool recursive;
    };
    std::vector<WatchCall> watchCalls;
    std::vector<std::string> unwatchCalls;
    std::vector<IFilesystemWatcher::Event> pendingEvents;

    bool watch(PathDomain, const char* path, bool recursive) override {
        watchCalls.push_back({path, recursive});
        return true;
    }
    void unwatch(PathDomain, const char* path) override {
        unwatchCalls.push_back(path);
    }
    std::vector<IFilesystemWatcher::Event> pollEvents() override {
        return std::exchange(pendingEvents, {});
    }
};

struct MockWindow : public IWindow {
    int logW{1280};
    int logH{720};
    int physW{1280};
    int physH{720};
    bool fullscreen{false};
    int lastSetW{0};
    int lastSetH{0};
    int setSizeCount{0};
    int setFullscreenCount{0};

    // Scripted folder-dialog result + observation (#665). `folderDialogResult` is what
    // showFolderDialog returns; leave it nullopt to simulate cancel/error. `folderDialogCalls`
    // counts invocations; `lastFolderDialogTitle`/`Location` capture the args.
    std::optional<std::string> folderDialogResult{};
    int folderDialogCalls{0};
    std::string lastFolderDialogTitle{};
    std::string lastFolderDialogLocation{};

    // Scripted message-box result + the args it was asked with (#1276).
    int buttonToReturn{0};
    std::string lastTitle{};
    std::string lastMessage{};
    std::string lastUrl{};

    bool init(const char*, int w, int h) override {
        logW = w;
        logH = h;
        physW = w;
        physH = h;
        return true;
    }
    void shutdown() override {}
    void pollEvents() override {}
    void setEventHandler(IWindowEventHandler*) override {}
    int width() const override {
        return physW;
    }
    int height() const override {
        return physH;
    }
    int logicalWidth() const override {
        return logW;
    }
    int logicalHeight() const override {
        return logH;
    }
    bool shouldClose() const override {
        return false;
    }
    void* nativeHandle() const override {
        return nullptr;
    }
    const char* getLastError() const override {
        return nullptr;
    }
    // Scripted message-box result + observation (#1276). buttonToReturn defaults to 0, which is what
    // this mock always returned before -- a suite that wants a different answer says so, the way
    // test_crash_reporter does when it dismisses a crash prompt.
    int showMessageBox(MessageBoxType, const char* title, const char* message, const MessageBoxButton*, int) override {
        lastTitle = title ? title : "";
        lastMessage = message ? message : "";
        return buttonToReturn;
    }
    void openURL(const char* url) override {
        lastUrl = url ? url : "";
    }
    std::optional<std::string> showFolderDialog(const char* title, const char* defaultLocation) override {
        ++folderDialogCalls;
        lastFolderDialogTitle = title ? title : "";
        lastFolderDialogLocation = defaultLocation ? defaultLocation : "";
        return folderDialogResult;
    }
    void setTitle(const char*) override {}
    bool setSize(int w, int h) override {
        ++setSizeCount;
        lastSetW = w;
        lastSetH = h;
        logW = w;
        logH = h;
        return true;
    }
    bool setFullscreen(bool fs) override {
        ++setFullscreenCount;
        fullscreen = fs;
        return true;
    }
    bool setDisplayMode(const IDisplay::DisplayMode&) override {
        return true;
    }
    int getCurrentMonitorId() const override {
        return 0;
    }
};

} // namespace fl
