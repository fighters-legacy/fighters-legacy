cmake_minimum_required(VERSION 3.25)
project(fighters-legacy VERSION 0.3.12 LANGUAGES C CXX)

execute_process(
    COMMAND git rev-parse --short HEAD
    OUTPUT_VARIABLE FL_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
if(NOT FL_GIT_HASH)
    set(FL_GIT_HASH "unknown")
endif()
configure_file(cmake/version.h.in engine/Version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# GameNetworkingSockets transport backend (#507). ON pulls protobuf (FetchContent) + OpenSSL
# (system) into the game/fl-server build. OFF yields an enet6-only build for lean tool/CI configs.
# Auto-disabled in dependencies.cmake if OpenSSL is not found.
option(FL_ENABLE_GNS "Build the GameNetworkingSockets transport backend (protobuf + OpenSSL)" ON)
# #935: the whisper.cpp speech-to-text backend for the deterministic voice-command tier.
# OFF by default — it is a sizeable dependency and the feature also needs a model file the project
# does not ship, so a default-on build would cost every contributor build time for something they
# cannot use out of the box. OFF gets NullSpeechToText and the radio menu, which is the #769 path.
option(FL_ENABLE_WHISPER "Build the whisper.cpp speech-to-text backend (voice wingman commands)" OFF)

# libFuzzer harnesses (#708). OFF by default; ON only via the clang-only `fuzz` preset. Gates the
# top-level fuzz/ subdir, which needs a libFuzzer main (-fsanitize=fuzzer) and clang coverage
# instrumentation, so it must never enter the MSVC/GCC build matrix.
option(FL_BUILD_FUZZERS "Build libFuzzer harnesses (clang only; see fuzz/)" OFF)

# The fuzz preset inherits the debug build, which leaves toml++'s internal TOML_ASSERT active. That
# assert aborts on some malformed TOML the parsers are meant to reject via toml::parse_error — a
# library debug-assert, not a fighters-legacy bug (release defines NDEBUG and toml++ throws cleanly).
# Neutralize it for the whole fuzz build so the harnesses reach the real parse-error handling, with
# ASan/UBSan remaining the memory-safety arbiter. add_compile_options (not add_compile_definitions,
# which rejects a function-like macro name, nor a raw CMAKE_CXX_FLAGS -D, which ninja hands to
# /bin/sh unquoted and chokes on the parens) emits the -D as a single shell-quoted argument.
if(FL_BUILD_FUZZERS)
    add_compile_options("-DTOML_ASSERT(expr)=static_cast<void>(0)")
    # Vendored parsers (toml++, stb_image, tinygltf) use two standard-UB-but-benign idioms that
    # adversarial fuzz input legitimately reaches, and which only these two UBSan sub-checks flag:
    #   * unreachable      — toml++ marks "can't happen for valid input" switch defaults with
    #                        __builtin_unreachable (an optimization hint in release).
    #   * pointer-overflow — stb's `if (buf + n <= end)` reader idiom forms the past-the-end pointer
    #                        BEFORE the bounds check that immediately follows (no actual OOB access).
    #   * nonnull-attribute — stb passes a zero-length range with a null base to memcpy (memcpy(nullptr,
    #                        _, 0)), benign but technically UB against memcpy's nonnull parameter.
    # Disable ONLY these three sub-checks for the fuzz build. Engine code doesn't rely on these C
    # idioms, and every other UBSan check (integer/signed overflow, alignment, ...) plus ALL of ASan's
    # memory-safety checks (OOB read/write, use-after-free, real null deref) stay ON — so a genuine
    # fighters-legacy memory bug is still caught; only the vendored parsers' benign pedantry is quieted.
    add_compile_options(-fno-sanitize=unreachable -fno-sanitize=pointer-overflow -fno-sanitize=nonnull-attribute)
endif()

include(cmake/dependencies.cmake)
include(cmake/layering.cmake)
# Runs at the end of this directory scope — after every add_subdirectory() below, so all targets
# (including conditional ones) exist when the module-boundary rules are asserted.
cmake_language(DEFER CALL fl_assert_layering)

enable_testing()

add_subdirectory(engine)
add_subdirectory(platform)
add_subdirectory(game)
add_subdirectory(server)
add_subdirectory(tools)
add_subdirectory(tests)

if(FL_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
