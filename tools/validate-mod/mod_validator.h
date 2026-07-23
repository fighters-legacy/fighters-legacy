// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace fl {

struct ModValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct ModValidateOptions {
    bool checkLicenses{true};             // run validate-licenses when REUSE.toml is present
    std::vector<std::string> allowedSpdx; // extra allowed SPDX ids beyond the defaults
};

// Validate a whole content pack (#651): the manifest, an optional [files] SHA-256 table, the pack
// structure, and every asset through the SAME per-asset validators fl-base-pack CI runs — composed by
// linking their libs, never subprocessing. Findings are domain-prefixed (manifest:/entities:/...).
[[nodiscard]] ModValidationResult validateMod(const std::string& packDir, const ModValidateOptions& opts = {});

} // namespace fl
