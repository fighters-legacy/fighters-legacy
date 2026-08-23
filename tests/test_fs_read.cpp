// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::readFileToString / fl::readFileBytes (#1254).
//
// Eleven sites opened, sized, read and closed a file by hand, and exactly ONE of them -- LuaSandbox
// -- honoured what readFile returns. IFilesystem is explicit that readFile returns the count
// actually read; every other copy passed getFileSize() and left the buffer at that size, so a short
// read leaves a NUL-padded tail that the caller then parses as content.
//
// A local disk rarely short-reads a regular file, which is why nothing was broken. The short-read
// case below is the one the eleven copies disagreed about, so it is the one worth a fake for.

#include "util/FsRead.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// A filesystem that reports a file's full size but hands over only part of it -- a network mount,
// a signal-interrupted read, an archive that streams.
class ShortReadingFs final : public fl::IFilesystem {
  public:
    std::map<std::string, std::string> files;
    std::size_t deliverAtMost = SIZE_MAX;

    int openFile(fl::PathDomain, const char* path, bool) override {
        auto it = files.find(path);
        if (it == files.end())
            return -1;
        m_open[m_next] = it->first;
        return m_next++;
    }
    void closeFile(int handle) override {
        m_open.erase(handle);
    }
    std::size_t readFile(int handle, void* buffer, std::size_t size) override {
        auto it = m_open.find(handle);
        if (it == m_open.end())
            return 0;
        const std::string& data = files[it->second];
        const std::size_t n = std::min({size, data.size(), deliverAtMost});
        if (n > 0)
            std::memcpy(buffer, data.data(), n);
        return n;
    }
    std::size_t getFileSize(int handle) const override {
        auto it = m_open.find(handle);
        if (it == m_open.end())
            return 0;
        return files.at(it->second).size(); // the FULL size, as a real filesystem reports it
    }

    // Unused by these helpers.
    std::size_t writeFile(int, const void*, std::size_t) override {
        return 0;
    }
    bool seek(int, std::size_t, fl::SeekOrigin) override {
        return false;
    }
    bool fileExists(fl::PathDomain, const char*) const override {
        return false;
    }
    bool createDirectory(fl::PathDomain, const char*) override {
        return false;
    }
    bool renameFile(fl::PathDomain, const char*, const char*) override {
        return false;
    }
    std::vector<Entry> scanDirectory(fl::PathDomain, const char*) const override {
        return {};
    }

  private:
    std::map<int, std::string> m_open;
    int m_next = 1;
};

} // namespace

TEST_CASE("a whole file reads back exactly", "[fs_read]") {
    ShortReadingFs fs;
    fs.files["data/thing.toml"] = "name = \"viper\"\n";

    const auto text = fl::readFileToString(fs, fl::PathDomain::Assets, "data/thing.toml");
    REQUIRE(text.has_value());
    CHECK(*text == "name = \"viper\"\n");

    const auto bytes = fl::readFileBytes(fs, fl::PathDomain::Assets, "data/thing.toml");
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 15u);
    CHECK((*bytes)[0] == 'n');
}

TEST_CASE("a missing file is nullopt, not an empty string", "[fs_read]") {
    ShortReadingFs fs;
    CHECK_FALSE(fl::readFileToString(fs, fl::PathDomain::Assets, "nope.toml").has_value());
    CHECK_FALSE(fl::readFileBytes(fs, fl::PathDomain::Assets, "nope.toml").has_value());
}

TEST_CASE("an empty file is a present, empty result", "[fs_read]") {
    // The distinction matters: ModLoader treats "no manifest" as skip-this-mod and an empty one as
    // a parse failure it reports. Collapsing them would silently swallow a truncated manifest.
    ShortReadingFs fs;
    fs.files["empty.toml"] = "";

    const auto text = fl::readFileToString(fs, fl::PathDomain::Assets, "empty.toml");
    REQUIRE(text.has_value());
    CHECK(text->empty());

    const auto bytes = fl::readFileBytes(fs, fl::PathDomain::Assets, "empty.toml");
    REQUIRE(bytes.has_value());
    CHECK(bytes->empty());
}

TEST_CASE("a short read is truncated to what arrived, not NUL-padded", "[fs_read]") {
    // THE case. getFileSize says 15; the filesystem hands over 4. Ten of the eleven copies this
    // replaced would have returned "name" followed by eleven NUL bytes, and the TOML parser would
    // have been handed that as the file's contents.
    ShortReadingFs fs;
    fs.files["data/thing.toml"] = "name = \"viper\"\n";
    fs.deliverAtMost = 4;

    const auto text = fl::readFileToString(fs, fl::PathDomain::Assets, "data/thing.toml");
    REQUIRE(text.has_value());
    CHECK(text->size() == 4u);
    CHECK(*text == "name");
    CHECK(text->find('\0') == std::string::npos);

    const auto bytes = fl::readFileBytes(fs, fl::PathDomain::Assets, "data/thing.toml");
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 4u);
}

TEST_CASE("a read that delivers nothing yields an empty result, not a padded one", "[fs_read]") {
    ShortReadingFs fs;
    fs.files["data/thing.toml"] = "name = \"viper\"\n";
    fs.deliverAtMost = 0;

    const auto text = fl::readFileToString(fs, fl::PathDomain::Assets, "data/thing.toml");
    REQUIRE(text.has_value());
    CHECK(text->empty());
}
