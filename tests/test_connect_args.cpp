// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "ConnectArgs.h"

static constexpr uint16_t kDefault = 4778;

// Helper: reset to defaults before each call.
static bool parse(const char* arg, std::string& host, uint16_t& port) {
    host.clear();
    port = kDefault;
    return parseConnectArg(arg, host, port);
}

TEST_CASE("parseConnectArg: host and port") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("127.0.0.1:4779", host, port));
    CHECK(host == "127.0.0.1");
    CHECK(port == 4779);
}

TEST_CASE("parseConnectArg: host only - keeps caller default port") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("127.0.0.1", host, port));
    CHECK(host == "127.0.0.1");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: IPv6 bracket notation with port") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("[::1]:4779", host, port));
    CHECK(host == "::1");
    CHECK(port == 4779);
}

TEST_CASE("parseConnectArg: IPv6 bracket notation without port") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("[::1]", host, port));
    CHECK(host == "::1");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: hostname with port") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("example.com:4780", host, port));
    CHECK(host == "example.com");
    CHECK(port == 4780);
}

TEST_CASE("parseConnectArg: hostname only") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("example.com", host, port));
    CHECK(host == "example.com");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: max valid port 65535") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("host:65535", host, port));
    CHECK(host == "host");
    CHECK(port == 65535);
}

TEST_CASE("parseConnectArg: port 0 is invalid - treat whole arg as hostname") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("host:0", host, port));
    CHECK(host == "host:0");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: port out of range - treat whole arg as hostname") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("host:99999", host, port));
    CHECK(host == "host:99999");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: non-numeric suffix - treat whole arg as hostname") {
    std::string host;
    uint16_t port = kDefault;
    CHECK(parse("host:notaport", host, port));
    CHECK(host == "host:notaport");
    CHECK(port == kDefault);
}

TEST_CASE("parseConnectArg: empty string returns false") {
    std::string host;
    uint16_t port = kDefault;
    CHECK_FALSE(parseConnectArg("", host, port));
}

TEST_CASE("parseConnectArg: null returns false") {
    std::string host;
    uint16_t port = kDefault;
    CHECK_FALSE(parseConnectArg(nullptr, host, port));
}
