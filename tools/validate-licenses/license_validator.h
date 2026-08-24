// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ValidatorCli.h"

#include <string>
#include <vector>

namespace fl {

// The shared three-field shape (#1277); the name stays for every caller.
using LicenseValidationResult = ToolValidationResult;

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

} // namespace fl
