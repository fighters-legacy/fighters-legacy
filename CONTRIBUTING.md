# Contributing

Welcome! fighters-legacy is a large community project and contributions are warmly encouraged.
Please read the [Code of Conduct](https://github.com/fighters-legacy/.github/blob/main/CODE_OF_CONDUCT.md) and [Governance](GOVERNANCE.md) before contributing.

## Commit Messages

This project uses [Conventional Commits](https://www.conventionalcommits.org/). The scope and type
drive the PR title lint, the changelog and the release notes.

**Your commit subject is the changelog entry.** `CHANGELOG.md` is generated from conventional-commit
subjects by [git-cliff](https://git-cliff.org) when a release is cut — **do not edit it in a PR**.
Write the subject as a statement to a player or an operator, not a note to yourself; the full
explanation belongs in the commit body and the PR description, which is where it still lives. See
[development.md](docs/developer/development.md#your-commit-subject-is-the-changelog-entry) for
subjects that read well and subjects that do not, and [project-management.md](docs/developer/project-management.md) for the
release procedure.

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

## Modelling real aircraft

If you are contributing an aircraft — a mesh, a flight model, or the numbers behind either — read
[docs/legal/aircraft-likeness.md](docs/legal/aircraft-likeness.md) **first**. In short: real types may
be modelled, from **public-domain government sources only**; nothing may be traced, derived, or
converted out of another simulator or commercial 3D model; aerodynamic values are **derived, not
copied**; and every aircraft ships a `SOURCES.md` citing every number to a public document.

## License Headers

All new `.cpp` and `.h` files must begin with an SPDX identifier:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

This is machine-readable and avoids reproducing the full license text in every file.

## Asset provenance

**Declare where a creative asset came from, in the PR that adds it.** Art, music, campaign prose,
and voice recordings: state whether it is your own work, its CC0/compatible source, or — if any part
of it was generated — say so explicitly.

Per the ratified AI content policy (#932, decision record dated 2026-07-27 in
`docs/developer/architecture.md`), **shipped creative assets are human-authored or CC0 and are never
generative-AI output.** Content the engine generates at runtime — briefings, debriefs, radio
chatter, TTS speech — is a different thing: it is ephemeral, opt-in per server and per client, and
is never baked back into a shipped pack.

**Audio is the one exception, amended 2026-08-17 (#1200).** Shipped voice lines and music may be
model-generated, and only then, and only if all four of these hold: the model is open-weight and
self-hosted; the file is regenerable from a model, version, prompt/spoken text and seed recorded in
the pack's `SOURCES.md`, plus a committed generator script; the file is marked **CC0** in REUSE
metadata rather than the pack's `CC-BY-4.0`, because a generated work carries no copyright to
assert; and the pack README discloses which assets are generated, player-facing. Art, story
campaigns and mission prose are not covered by the exception.

The distinction that decides any given case is the artifact's **lifetime**, not how it was made: a
thing a player keeps is an asset, a thing that exists for one session is not. If you are unsure which
side of that line something falls on, ask in the PR rather than guessing — an asset with unclear
provenance is far more expensive to remove later than to question now.

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

New code added in PRs should aim for ≥70% test coverage. The `Coverage` workflow builds an
instrumented tree and runs the full suite on every pull request, so coverage is measured in CI
automatically — no local setup required. Codecov posts a comment showing the delta for the files
you changed, and the run's job summary carries the measured `engine/` percentages against the
gates (branch ≥ 80%, line ≥ 70%) plus the lowest-covered files when one is missed.

The coverage run is **advisory**: it is not a required check, so a red one does not block a merge.
Read it, and either add the tests or say in the PR why the drop is right. If it fails without a
number — the message says "no coverage number was produced" — that is a broken measurement, not a
verdict on your change; please flag it rather than working around it.

> **The `engine/` branch gate is currently failing, and it is not your PR.** Branch coverage is
> 75.09% against the 80% threshold. The gate spent five weeks unable to measure anything (#1128),
> and most of the engine was written in that window; the shortfall and where it lives are tracked in
> **#1145**. The threshold is deliberately staying at 80 rather than being moved down to meet it, so
> expect this check to be red until #1145 closes. Judge your own change by the Codecov delta on the
> files you touched.

The project targets meaningful coverage on logic-bearing code. Trivial getters, generated code,
and platform-specific HAL shims are excluded from the threshold.

## First-Time Contributor Guide

1. Fork the repository and clone locally
2. Create a branch: `git checkout -b feat/your-feature`
3. Install prerequisites and build: see [docs/developer/development.md](docs/developer/development.md)
4. Install the DCO hook: `cp scripts/hooks/commit-msg .git/hooks/ && chmod +x .git/hooks/commit-msg`
5. Make your changes and add tests
6. Sign off and commit: `git commit -s -m "feat(scope): description"` — the subject is published
   verbatim as the changelog entry, so write it for a reader of the release notes. Do not edit
   `CHANGELOG.md`
7. Open a pull request against `main` and fill in the PR template

For full build workflow, IDE setup, and the release process, see [docs/developer/development.md](docs/developer/development.md). For upstream documentation on every technology used in the engine, see [docs/developer/references.md](docs/developer/references.md).
