# Project Management

How work is planned, tracked, and prioritized in this repository. It documents the
GitHub-native system this project evolved — issue **types**, **epics** with sub-issues,
**labels**, **milestones**, and a single **Project board** — so contributors have one
reference and so any other project can adopt the same model as a proven baseline.

> This describes *this* repository's process. The companion repos
> (`fl-account`, `fl-review`, `fl-operator`) may adopt it as-is. Nothing here overrides
> [GOVERNANCE.md](../GOVERNANCE.md) (roles, decision-making) or
> [CONTRIBUTING.md](../CONTRIBUTING.md) (commit, DCO, and PR rules) — this doc ties them together.

## Philosophy

Everything lives on GitHub: issues with **types**, a **Project board** for status and
prioritization, **milestones** for phases, and **labels** for component routing. During
pre-1.0 development we favor velocity — architectural changes are captured as dated
**decision records** in [architecture.md](architecture.md#decision-records) rather than
heavyweight RFCs, with the RFC process reserved for community-facing and post-freeze changes
(see [GOVERNANCE.md](../GOVERNANCE.md)).

## Work hierarchy

Work is organized along **two independent axes**:

- **Phase** = *when* — a [milestone](#milestones). One milestone per phase.
- **Epic** = *which initiative* — a long-lived, cross-cutting thread (the "Multiplayer at
  Scale" epics A–L in [roadmap.md](roadmap.md) span multiple phases).

A unit of work is an **issue** with a [type](#issue-types). Large initiatives are an **Epic**
issue that decomposes into **sub-issues** using GitHub's native parent/sub-issue linking:

```
Phase (milestone)  ─────────────────────────────────  the "when"
        │
        ├─ Epic #468  spherical-Earth simulation  ───  the "what" (an initiative)
        │     ├─ sub-issue #469  (Feature)
        │     ├─ sub-issue #470  (Task)
        │     └─ … #471–490        (Spike / Bug / …)
        │
        └─ standalone Feature / Task / Bug          ─  not every issue needs an epic
```

Epic **#468** (sub-issues **#469–490**) is the canonical worked example. Create sub-issues
from the parent issue's "Create sub-issue" control in the GitHub UI, or via tooling
(`gh`/the GitHub MCP `sub_issue_write`). The Project board's **Sub-issues progress** field
then tracks epic completion automatically.

Sub-issues may live in a **different repository within the org**: epic #54 (fl-base-pack
initial content) parents the `fl-base-pack` repo's issues, so the board rollup tracks
content-pack readiness — the fl-base-pack half of the Phase 4 gate — from the engine repo's
board. Use the issue URL as the parent reference when linking across repos.

## Issue types

Issue types are enabled org-wide and are the **source of truth for what kind of work an
issue is**. Set the type on every new issue.

| Type | Use for |
|---|---|
| **Epic** | A large, multi-issue initiative tracked via sub-issues. |
| **Feature** | A new capability, request, or idea. |
| **Task** | A specific, well-scoped piece of work (incl. docs/chore work). |
| **Spike** | A time-boxed investigation that *informs* follow-on work, rather than shipping a feature directly. |
| **Bug** | An unexpected problem or incorrect behavior. |

A **Spike** produces a decision or a set of follow-on issues, not a finished feature — close
it once its question is answered and the follow-ons are filed (e.g. the Epic A design spike
that preceded the job-system work).

## Labels

Labels route and filter; they do **not** duplicate the issue type. Three families
(canonical source: [`.github/labels.yml`](../.github/labels.yml)):

- **`component: *`** — the subsystem an issue touches (`engine`, `renderer`, `network`,
  `netcode`, `ai`, `flight`, `content`, `server`, `tools`, …). These are **auto-applied to
  PRs** from changed file paths by [`.github/labeler.yml`](../.github/labeler.yml), and they
  mirror the **scope** in a conventional-commit subject (`feat(network): …`). Apply the
  matching `component:` label to issues at triage.
- **RFC workflow** — `rfc` plus a `status:` lifecycle (`under-discussion` → `accepted` /
  `rejected` → `implemented`). **RFCs are a label-driven workflow, not an issue type** — an
  RFC is a Feature or Task carrying the `rfc` label.
- **Standard / meta** — `documentation`, `good first issue`, `help wanted`, `needs-info`,
  `release`, `backlog`, plus GitHub defaults.

**Type vs. label.** The issue **type** (Feature/Bug/…) is authoritative for the kind of
work. The older `enhancement` / `bug` *labels* are retained for back-compat and for GitHub's
default filters, so an issue may carry both `Feature` (type) and `enhancement` (label) — the
type wins. (See [Lessons & Rev 2](#lessons--rev-2).)

## Milestones

**One milestone per phase** — Phase 3 (active) through Phase 9. The milestone answers *when*
a piece of work is scheduled; phase gating (a phase depends on prior phases) is described in
[roadmap.md](roadmap.md). Assign every issue to its phase milestone at triage. Items with no
scheduled phase get the `backlog` label instead.

**Epics carry the milestone of their *finish* phase** — the phase of their last open
sub-issue. Epics span phases (their sub-issues keep their own per-phase milestones), so an
epic whose decomposition extends into a later phase is re-homed forward rather than left
blocking an earlier milestone or closed with open subs (convention set 2026-07-01 with
#494/#496, applied to #588–#592).

**Due dates** are set only on the active phase's milestone and on externally anchored gates
(Phase 4 carries the fl-base-pack readiness date). Later phases are sequentially gated, not
date-driven — their milestones stay dateless.

## The Project board

A single org Project, **"Fighters Legacy 1.0"**, holds every open item. New issues and PRs
are **auto-added** to it (the Project's built-in *Auto-add to project* workflow), so the
board is the complete picture without manual curation.

**Three views, each for a different job:**

| View | Layout | Used for |
|---|---|---|
| **Roadmap** | Timeline | Scheduling across time, driven by the **Start Date** / **Target Date** fields. |
| **Board** | Kanban | Day-to-day flow, grouped by the **Status** field. |
| **Open Items** | Table | Triage and bulk editing across all fields. |

**Fields:**

- **Status** — `Todo` → `In Progress` → `Done`. The kanban columns.
- **Effort** — single-select size estimate (see [Lessons & Rev 2](#lessons--rev-2)).
- **Order** — the explicit implementation-order ranking (number field), two layers
  (convention set 2026-07-01):
  - **Epics: `1–N`** — one unified initiative sequence across all open epics, in planned
    implementation-start order. Derived from the roadmap's dependency records (e.g. the
    scaling spine finishes first; mission runtime → weapons; M precedes N/O/P; H→C→D with
    G alongside and K last). Epics sort to the top of an Order-sorted view as initiative
    headers.
  - **Work items: phase bands** — the thousands digit encodes the phase sequence: the
    active phase uses small numbers (`10+`, step 10), then Phase 4 = `1000s` (step 5,
    largest band), Phase 5 = `2000s`, Phase 6 = `3000s`, Phase 7 = `4000s`,
    Phase 8 = `5000s`, Phase 9 = `6000s` (step 10). An item's band always matches its own
    milestone. Within a band, items group into blocks following the epic sequence, and
    within a block follow the epic's curated sub-issue order; standalones are slotted by
    dependency. Gaps allow insertion without renumbering; re-band items when they change
    milestone.
- **Start Date** / **Target Date** — drive the Roadmap timeline.
- **Parent issue** / **Sub-issues progress** — epic decomposition and rollup.
- **Milestone**, **Labels**, **Assignees** — mirrored from the issue.

## Triage checklist

When opening or grooming an issue, set all of:

- [ ] **Type** — Epic / Feature / Task / Spike / Bug.
- [ ] **Milestone** — the phase it belongs to (or `backlog` label if unscheduled).
- [ ] **`component:` label(s)** — the subsystem(s) it touches.
- [ ] **Project** — confirm it's on the board (auto-add handles new issues).
- [ ] **Parent** — link it under its Epic if it's part of one.
- [ ] **Status** — `Todo` until picked up.

## Decision records and RFCs

- **Decision records** — during pre-1.0 development, architectural decisions are recorded as
  dated entries in [architecture.md](architecture.md#decision-records), format:
  `**YYYY-MM-DD — <Title> (<Epic>, #<Issue>).** <rationale>`. This is the lightweight,
  high-velocity path for internal architecture.
- **RFCs** — required for public-API, cross-module-architecture, new-major-dependency, and
  community-facing-format changes. Open one with the **RFC issue template**, label it `rfc` +
  `status: under-discussion`, and follow the process in [GOVERNANCE.md](../GOVERNANCE.md).

Promote a decision record to a full RFC once the wire protocol / public API freezes (post-1.0).

## Issue → branch → PR → merge

The delivery loop, in brief (full rules in [CONTRIBUTING.md](../CONTRIBUTING.md)):

1. **Branch** off `main`: `<type>/<short-kebab-description>`.
2. **Commit** with [Conventional Commits](https://www.conventionalcommits.org/) — the
   `<scope>` mirrors the issue's `component:` label — and a DCO sign-off (`git commit -s`).
3. **PR** referencing the issue (`Closes #NNN`); the title is conventional-commit form
   (enforced by `pr-title-lint`), sign-off is enforced by `dco`, and the
   `component:` labels are applied automatically by `labeler`.
4. **CHANGELOG** — add an entry under `[Unreleased]` (`### Added` / `### Fixed` /
   `### Changed`) in [CHANGELOG.md](../CHANGELOG.md). Required for every PR.
5. **Merge** once CI is green on all three platforms.

## Adopting this in a new project

A copy-this checklist to stand up the same system in a fresh repo:

1. **Enable issue types** for the org (Settings → Issue types): Epic, Feature, Task, Spike,
   Bug. Do this *before* filing issues so there's no untyped tail.
2. **Create the label set** — start from [`.github/labels.yml`](../.github/labels.yml); keep
   the `component: *` taxonomy aligned with your commit scopes.
3. **Wire path-based labeling** — copy [`.github/labeler.yml`](../.github/labeler.yml) and
   the `labeler` workflow before the first PR.
4. **Add issue & PR templates** — [`.github/ISSUE_TEMPLATE/`](../.github/ISSUE_TEMPLATE/)
   (bug, feature, rfc, epic, spike) and `PULL_REQUEST_TEMPLATE.md`.
5. **Create one org Project** with the three views (Roadmap / Board / Open Items) and the
   fields above. **Define the Effort options up front.** Enable the **Auto-add to project**
   workflow so nothing is missed.
6. **Create a milestone per phase**; assign every issue to its phase.
7. **Enforce in CI** — conventional PR titles, DCO sign-off, and (if licensing) REUSE/SPDX.

## Lessons & Rev 2

What worked, and what we would change starting from scratch:

**Keep:**

- **Milestones-as-phases + cross-cutting epics** is a clean two-axis model (*when* vs. *which
  initiative*) and scaled well across nine phases.
- **`component:` labels + path-based labeler + matching commit scopes** kept routing
  effortless and PRs self-labeling. Seed `labeler.yml` before the first PR.
- **Pre-1.0 dated decision records** (instead of full RFCs) were the right velocity tradeoff,
  with a clear "promote to RFC at freeze" rule.
- **Native sub-issues + the Sub-issues-progress field** made epic rollup automatic.

**Change on day one next time:**

- **Define the `Effort` options at project creation.** The field exists here but was never
  given options, so sizing/velocity tracking never happened. Pick a scale (T-shirt `XS`–`XL`
  or Fibonacci) up front and apply it going forward.
- **Adopt issue types before filing issues.** Types were enabled mid-project, leaving a tail
  of untyped issues to backfill. Enable them first.
- **Decide the type-vs-label policy explicitly.** Enabling types after the `enhancement` /
  `bug` labels already existed created lasting redundancy. Choose types as the source of
  truth and either drop the redundant default labels or keep them only for GitHub's filters.
- **Enable the Project's Auto-add workflow at creation.** Board membership was manual for much
  of the project, so issues silently missed the board until this was turned on.
- **Use the `Order` / priority field deliberately.** Priority lived implicitly in epic
  sequencing and the critical path for the first three phases; the explicit two-layer
  ranking (unified epic sequence + phase-banded work items, see
  [Fields](#the-project-board)) was only adopted 2026-07-01. Adopt it from day one next
  time — retrofitting meant renumbering the whole board once phases were re-planned.
