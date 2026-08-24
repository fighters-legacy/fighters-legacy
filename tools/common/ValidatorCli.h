// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The scaffolding behind every validate-* CLI (#1277).
//
// Twelve tools shipped the same contract -- exit 0/1/2, `--help`/`-h`, `--version`/`-v`, a
// "WARN  "/"ERROR " report loop -- from twelve independent copies, and ten of them declared the
// same three-field result struct under ten different names. That is twelve-way dual maintenance of
// a DOCUMENTED, shipped interface: the exit codes appear in every help text and the tool list is a
// docs_drift surface. Nothing had drifted yet on the contract, but the report format already had
// (mission/sensor print `WARN  [file]`, mesh/mod print `WARN  `), and a deliberate difference is
// indistinguishable from an accidental one once there are twelve places to look.
//
// Header-only and stdlib-only, beside NetStats.h. Deliberately NOT a framework: the argument loops
// that genuinely differ (validate-mod's dir+options, validate-licenses' four flags,
// validate-playlist's --playlist) keep their own, because a template that owned every arg loop
// would overfit the real variance. What lives here is what is actually the same twelve times.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fl {

// The result shape ten of the validators declared for themselves. They are aliases of this now, so
// a change to the shape is one edit rather than ten -- and `merge<R>()` in mod_validator, which
// duck-types across all of them, keeps working unchanged.
//
// "Tool" in the name is load-bearing: engine/content/AssetValidator.h already owns fl::Validation-
// Result, a {valid, reason} pair for asset magic-byte and size limits. Different question, different
// type -- and validate-mod links engine-content, so the two names have to coexist in one TU.
//
// PlaylistValidationResult is deliberately NOT an alias: its `ok` is a method deriving from
// `errors.empty()`, and it carries stateCount/totalTracks. Its contract really is different.
// MissionValidationResult stays in engine/mission for the same reason it moved there (#601) --
// engine cannot include tools/.
struct ToolValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Every validator reports this. One constant so a release cannot bump eleven and miss the twelfth.
inline constexpr const char* kValidatorVersion = "0.0.1";

// The `--help`/`-h` and `--version`/`-v` prefix every main opens with. Returns true when it printed
// something and main should return *exitCode*.
template <typename PrintHelp>
[[nodiscard]] inline bool handledHelpOrVersion(const char* arg, const char* toolName, PrintHelp printHelp,
                                               int& exitCode) {
    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
        printHelp();
        exitCode = 0;
        return true;
    }
    if (std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-v") == 0) {
        std::printf("%s %s\n", toolName, kValidatorVersion);
        exitCode = 0;
        return true;
    }
    return false;
}

// The whole file as a string, in TEXT mode -- which normalises CRLF on Windows, and is what every
// main has always done with its input. Empty optional means it could not be opened, which every
// validator reports as a validation failure (exit 1), not a usage error (exit 2): a path the author
// got wrong is something to fix in the pack, and the run should carry on to the remaining files.
//
// engine/util/FsRead.h has a same-named overload over IFilesystem. That one is for engine code
// reading through the HAL; this one is for a tool reading the developer's own disk directly.
[[nodiscard]] inline std::optional<std::string> readFileToString(const char* path) {
    std::ifstream f(path);
    if (!f)
        return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The whole file in BINARY mode, empty string when it cannot be opened. That is the shape the
// pack walkers want: they hand the result straight to a parser and let the failure surface as the
// parse error it becomes, rather than branching twice on the same problem.
//
// Binary is not incidental. validate-licenses reads in TEXT mode ON PURPOSE, to normalise CRLF
// before matching SPDX tags line by line; the pack walkers must not have bytes rewritten under a
// parser. Two functions, because it is two decisions.
[[nodiscard]] inline std::string readFileBinary(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Warnings, then errors, then the exit code. A null `label` prints without the "[file] " prefix --
// validate-mesh and validate-mod report against a pack rather than a file and have nothing to put
// there. Both formats are preserved exactly; this is not the place to unify them.
//
// Templated on the result because MissionValidationResult is a distinct type in engine/mission, and
// because ToolValidationResult's aliases must not become an overload set.
template <typename Result> inline void reportResult(const Result& result, const char* label, int& exitCode) {
    for (const auto& w : result.warnings) {
        if (label)
            std::fprintf(stderr, "WARN  [%s] %s\n", label, w.c_str());
        else
            std::fprintf(stderr, "WARN  %s\n", w.c_str());
    }
    for (const auto& e : result.errors) {
        if (label)
            std::fprintf(stderr, "ERROR [%s] %s\n", label, e.c_str());
        else
            std::fprintf(stderr, "ERROR %s\n", e.c_str());
    }
    if (!result.ok)
        exitCode = 1;
}

// Walks argv[firstArg..] as a list of input paths: anything starting with '-' is a usage error
// (exit 2, matching every tool today), and each remaining argument goes to
// `validateOne(path, exitCode)`. Returns the exit code.
template <typename ValidateOne>
[[nodiscard]] inline int runFileList(int argc, char* argv[], int firstArg, ValidateOne validateOne) {
    int exitCode = 0;
    for (int i = firstArg; i < argc; ++i) {
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
        validateOne(argv[i], exitCode);
    }
    return exitCode;
}

// The commonest validateOne: slurp the file, validate its CONTENTS, report under the file name.
// validate-mesh is the exception -- it validates a path, because a .glb is binary and tinygltf
// opens it itself -- so it passes its own lambda to runFileList instead.
template <typename Validate> [[nodiscard]] inline auto validateFileContents(Validate validate) {
    return [validate](const char* path, int& exitCode) {
        const std::optional<std::string> contents = readFileToString(path);
        if (!contents) {
            std::fprintf(stderr, "error: cannot open %s\n", path);
            exitCode = 1;
            return;
        }
        reportResult(validate(*contents), path, exitCode);
    };
}

} // namespace fl
