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

The current list of committers is maintained in [CONTRIBUTORS.md](CONTRIBUTORS.md) once it is created.

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

## Code of Conduct Enforcement

Enforcement is handled by the Project Lead. Appeals may be sent to **fighters-legacy@mkz.io**. The enforcement ladder follows the [Code of Conduct](https://github.com/fighters-legacy/.github/blob/main/CODE_OF_CONDUCT.md):

1. Correction
2. Warning
3. Temporary ban
4. Permanent ban
