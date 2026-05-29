// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

struct LicenseValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates REUSE 1.0 license compliance for a content pack directory.
//
// Checks:
//   - REUSE.toml exists and its project-wide annotation uses an allowed SPDX identifier
//   - Per-file .license sidecars have SPDX-License-Identifier and SPDX-FileCopyrightText
//   - All referenced SPDX identifiers are in allowedSpdxIds
//   - LICENSES/<id>.txt exists for every identifier referenced in .license sidecars
//   - Inline SPDX-License-Identifier comments in non-binary text files use allowed identifiers
//
// licensesDir: path to the LICENSES/ directory (pass empty string to skip cross-reference check)
LicenseValidationResult validateLicenses(const std::string& rootDir, const std::vector<std::string>& allowedSpdxIds,
                                         const std::string& licensesDir);
