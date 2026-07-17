// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "IWindow.h"
#include "mock_hal.h"

#include <optional>
#include <string>

using namespace fl;

namespace {

// A minimal IWindow that does NOT override showFolderDialog — it locks the base-class
// default-implementation contract (#665): a backend without a native picker returns nullopt.
struct NoPickerWindow : public IWindow {
    bool init(const char*, int, int) override {
        return true;
    }
    void shutdown() override {}
    void pollEvents() override {}
    void setEventHandler(IWindowEventHandler*) override {}
    int width() const override {
        return 0;
    }
    int height() const override {
        return 0;
    }
    int logicalWidth() const override {
        return 0;
    }
    int logicalHeight() const override {
        return 0;
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
    int showMessageBox(MessageBoxType, const char*, const char*, const MessageBoxButton*, int) override {
        return -1;
    }
    void openURL(const char*) override {}
    void setTitle(const char*) override {}
    bool setSize(int, int) override {
        return false;
    }
    bool setFullscreen(bool) override {
        return false;
    }
    bool setDisplayMode(const IDisplay::DisplayMode&) override {
        return false;
    }
    int getCurrentMonitorId() const override {
        return -1;
    }
};

} // namespace

TEST_CASE("IWindow::showFolderDialog default returns nullopt (no native picker)") {
    NoPickerWindow w;
    IWindow& iw = w;
    CHECK_FALSE(iw.showFolderDialog("Pick a folder", nullptr).has_value());
}

TEST_CASE("MockWindow::showFolderDialog returns the scripted path and records the call") {
    MockWindow w;
    w.folderDialogResult = std::string("/home/pilot/Fighters Anthology");

    IWindow& iw = w;
    const auto picked = iw.showFolderDialog("Locate FA", "/home/pilot");

    REQUIRE(picked.has_value());
    CHECK(*picked == "/home/pilot/Fighters Anthology");
    CHECK(w.folderDialogCalls == 1);
    CHECK(w.lastFolderDialogTitle == "Locate FA");
    CHECK(w.lastFolderDialogLocation == "/home/pilot");
}

TEST_CASE("MockWindow::showFolderDialog maps an unset scripted result to nullopt (cancel)") {
    MockWindow w; // folderDialogResult left unset

    const auto picked = w.showFolderDialog("Locate FA", nullptr);

    CHECK_FALSE(picked.has_value());
    CHECK(w.folderDialogCalls == 1);
    CHECK(w.lastFolderDialogLocation.empty()); // nullptr defaultLocation captured as ""
}
