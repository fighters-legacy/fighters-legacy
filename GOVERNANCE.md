# Governance

## Project Roles

### Project Lead (Benevolent Dictator for Life)

`@jomkz` holds the Project Lead role. In open-source governance, "BDFL" (Benevolent Dictator for Life) is a common term meaning a single trusted maintainer holds final decision authority while actively welcoming broad community input.

Responsibilities:
- Final decision authority on all technical and community matters
- Merging pull requests to `main`
- Granting and revoking Committer status
- Enforcing the Code of Conduct

### Committers

Committers can review and merge pull requests within their designated module scope. Committer status is granted by the Project Lead after demonstrated sustained quality contributions.

**Committer ladder:**
- **Module committer** — 5+ merged PRs of consistent quality in a specific module; merge rights scoped to that module
- **Global committer** — sustained multi-module contributions over time; unrestricted merge rights
- **Emeritus** — 12+ months inactive; retains title, no active merge rights

The current list of committers is maintained in [AUTHORS](AUTHORS).

### Contributors

Anyone who submits a pull request, files an issue, or participates in Discussions is a Contributor. Contributors are recognized in release notes.

## Decision Making

### Lazy Consensus

Ordinary changes (bug fixes, documentation, non-breaking features) proceed if no objections are raised within 72 hours of a pull request being opened for review. Silence is consent.

### Explicit Approval

The following require explicit approval from the Project Lead plus at least one Committer (or sole Project Lead if no Committers exist yet):

- Breaking API or ABI changes
- New major dependencies
- License changes
- Significant cross-module architectural changes

### RFC Process

Required for:
- Changes to public API surfaces
- Cross-module architectural decisions
- New major dependencies
- Changes to community-facing formats (mod manifests, content pack interface)

**Process:**
1. Open an issue using the [RFC template](.github/ISSUE_TEMPLATE/rfc.yml)
2. Minimum 14-day discussion period before a decision is made
3. Decision recorded in the issue with `status: accepted` or `status: rejected` label
4. Accepted RFCs become tracking issues linked to implementation PRs

### Decision Records (primary development)

During primary development — before the `kProtocolVersion` / 1.0 freeze — the Project Lead may
revise a previously-locked architectural decision via a **dated decision record** instead of
the full 14-day RFC, provided the change and its rationale are recorded in
[docs/developer/architecture.md](docs/developer/architecture.md#decision-records). This keeps pre-1.0 architecture
velocity without leaving the locked-decisions table silently stale. Once the protocol freezes,
the full RFC process is required for the same classes of change. The 2026-06-28 re-target to
128+ multiplayer is the first such record.

## Content Pack Linking Exception

Fighters Legacy is licensed under the GNU General Public License, version 3 or later
(GPL-3.0-or-later). The following exception grants additional permission for the narrow purpose of
implementing content packs, so that mods and plugins — including proprietary or paid content — can
link against the engine's stable content-pack interface without being required to be GPL-licensed
themselves. This is the exception the `IContentPack.h` header refers to, and it is a hard
prerequisite for the premium-content distribution mechanism.

### Grant of additional permission

As an exception to the terms of the GPL-3.0-or-later, the copyright holders of Fighters Legacy give
you permission to combine Fighters Legacy with a work (the "Content Pack") that implements the
Fighters Legacy content-pack interface, and to convey the resulting combined work, under terms of
your choice for the Content Pack, provided that:

1. The only Fighters Legacy source you incorporate into the Content Pack is limited to the
   **Vendorable Interface Set** (defined below), used solely to implement the content-pack
   interface; and
2. You comply with the GPL-3.0-or-later for Fighters Legacy itself in all other respects — in
   particular, any modification you make to Fighters Legacy outside the Vendorable Interface Set,
   and any conveyance of Fighters Legacy itself, remains governed by the GPL-3.0-or-later; and
3. The Content Pack does not require, and is not distributed together in a single combined binary
   with, GPL-licensed Fighters Legacy object code except as the GPL already permits.

### The Vendorable Interface Set

The exception covers linking against, including, and vendoring **only** the following headers, which
together define the stable interface a content pack implements:

- `engine/content/IContentPack.h`
- `engine/content/AssetTypes.h`
- `engine/content/TrustLevel.h`

These headers are pure interface (abstract classes, POD types, and enumerations) and carry no
engine implementation. A downstream project may copy or vendor them (as fa-bridge does via its
`extern/fl-headers` pin) to build an out-of-tree content pack.

### What "link against" permits, and what remains GPL

- **Permitted for the Content Pack under its own terms:** implementing the interfaces declared in
  the Vendorable Interface Set; including/vendoring those headers; and distributing the resulting
  content pack (loaded at runtime by the engine as a plugin or data pack) under any license,
  including a proprietary or paid one.
- **Still governed by the GPL-3.0-or-later:** the Fighters Legacy engine, game client, server,
  tools, and every other header and source file **not** listed in the Vendorable Interface Set.
  Modifying the engine, statically linking any GPL engine code beyond the Vendorable Interface Set
  into a proprietary binary, or conveying the engine itself, all remain subject to the GPL.

### Withdrawal and versioning

This exception applies to the versions of the Vendorable Interface Set headers distributed with the
Fighters Legacy releases that carry it. If a future release changes the Vendorable Interface Set,
the exception text distributed with that release governs it. The Project Lead may add headers to the
Vendorable Interface Set (an interface addition is not a withdrawal); narrowing the set is a license
change and follows the Explicit Approval process above.

## Code of Conduct Enforcement

Enforcement is handled by the Project Lead. Appeals may be sent to **fighters-legacy@mkz.io**. The enforcement ladder follows the [Code of Conduct](https://github.com/fighters-legacy/.github/blob/main/CODE_OF_CONDUCT.md):

1. Correction
2. Warning
3. Temporary ban
4. Permanent ban
