# Contributing

Welcome! fighters-legacy is a large community project and contributions are warmly encouraged.
Please read the [Code of Conduct](CODE_OF_CONDUCT.md) and [Governance](GOVERNANCE.md) before contributing.

## Commit Messages

This project uses [Conventional Commits](https://www.conventionalcommits.org/). Commit messages
drive automated changelog generation via `git-cliff` and `scripts/draft-changelog.sh`.

### Format

```
<type>[(<scope>)][!]: <description>
```

**Examples:**
```
feat(renderer): add Vulkan swapchain initialisation
fix(network): correct ENet packet fragmentation threshold
docs: document IContentPack interface
refactor(engine): extract asset manager into separate translation unit
feat(content)!: change mod manifest format — breaks existing mods
```

### Types

| Type | Changelog section | When to use |
|---|---|---|
| `feat` | Added | New user-facing functionality |
| `fix` | Fixed | Bug fixes |
| `docs` | Changed | Documentation only |
| `refactor` | Changed | Code restructuring, no behaviour change |
| `perf` | Changed | Performance improvements |
| `chore` | *(omitted)* | Maintenance, dependency bumps |
| `ci` | *(omitted)* | CI/CD changes |
| `build` | *(omitted)* | Build system changes |
| `test` | *(omitted)* | Adding or updating tests |
| `style` | *(omitted)* | Formatting, whitespace |

### Scopes

| Scope | Targets |
|---|---|
| `engine` | `engine/` — HAL, content system, asset manager |
| `renderer` | `platform/vulkan/` — Vulkan renderer backend |
| `audio` | `platform/openal/` — OpenAL Soft audio backend |
| `network` | `platform/net/` — ENet networking backend |
| `content` | `engine/content/` — IContentPack, ModLoader |
| `flight` | Flight model |
| `ai` | Lua AI runtime |
| `mission` | Mission / campaign loader |
| `build` | CMake build system |
| `ci` | GitHub Actions workflows |
| `docs` | Documentation |

Omit the scope when a change spans multiple components. Do not combine scopes — split into separate commits or drop the scope entirely.

### Breaking Changes

Append `!` after the type/scope, or add a `BREAKING CHANGE:` footer:

```
feat(content)!: rename IContentPack::load() to IContentPack::fetch()

BREAKING CHANGE: all content pack implementations must rename the method.
```

### Branch Names

```
<type>/<short-kebab-description>
```

Examples: `feat/vulkan-swapchain`, `fix/enet-packet-fragmentation`, `docs/architecture-overview`

## License Headers

All new `.cpp` and `.h` files must begin with an SPDX identifier:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

This is machine-readable and avoids reproducing the full license text in every file.

## Developer Certificate of Origin (DCO)

This project uses the [Developer Certificate of Origin](https://developercertificate.org/) instead of a CLA.
By contributing, you certify that you have the right to submit the work under this project's license.

Sign off every commit with `-s`:

```bash
git commit -s -m "feat(engine): add HAL interface"
```

This appends `Signed-off-by: Your Name <you@example.com>` to your commit message. DCO sign-off is
enforced by CI — unsigned commits will block the PR.

**Tip:** Install the provided commit-msg hook to sign off automatically:

```bash
cp scripts/hooks/commit-msg .git/hooks/
chmod +x .git/hooks/commit-msg
```

## Code Coverage

New code added in PRs should aim for ≥70% test coverage. Codecov posts an automated comment on
every PR showing the coverage delta for changed files. Coverage is measured in CI automatically —
no local setup required.

The project targets meaningful coverage on logic-bearing code. Trivial getters, generated code,
and platform-specific HAL shims are excluded from the threshold.

## First-Time Contributor Guide

1. Fork the repository and clone locally
2. Create a branch: `git checkout -b feat/your-feature`
3. Install prerequisites and build: see [docs/development.md](docs/development.md)
4. Install the DCO hook: `cp scripts/hooks/commit-msg .git/hooks/ && chmod +x .git/hooks/commit-msg`
5. Make your changes and add tests
6. Update `CHANGELOG.md` under `[Unreleased]`
7. Sign off and commit: `git commit -s -m "feat(scope): description"`
8. Open a pull request against `main` and fill in the PR template

For full build workflow, IDE setup, and the release process, see [docs/development.md](docs/development.md). For upstream documentation on every technology used in the engine, see [docs/references.md](docs/references.md).
