# Project Management

How work is planned, tracked, and prioritized in this repository. It documents the
GitHub-native system this project evolved — issue **types**, **epics** with sub-issues,
**labels**, **milestones**, and a single **Project board** — so contributors have one
reference and so any other project can adopt the same model as a proven baseline.

> This describes *this* repository's process. The companion repos
> (`fl-account`, `fl-review`, `fl-operator`) may adopt it as-is. Nothing here overrides
> [GOVERNANCE.md](https://github.com/fighters-legacy/fighters-legacy/blob/main/GOVERNANCE.md) (roles, decision-making) or
> [CONTRIBUTING.md](https://github.com/fighters-legacy/fighters-legacy/blob/main/CONTRIBUTING.md) (commit, DCO, and PR rules) — this doc ties them together.

## Philosophy

Everything lives on GitHub: issues with **types**, a **Project board** for status and
prioritization, **milestones** for phases, and **labels** for component routing. During
pre-1.0 development we favor velocity — architectural changes are captured as dated
**decision records** in [architecture.md](architecture.md#decision-records) rather than
heavyweight RFCs, with the RFC process reserved for community-facing and post-freeze changes
(see [GOVERNANCE.md](https://github.com/fighters-legacy/fighters-legacy/blob/main/GOVERNANCE.md)).

## Work hierarchy

Work is organized along **two independent axes**:

- **Phase** = *when* — a [milestone](#milestones). One milestone per phase.
- **Epic** = *which initiative* — a long-lived, cross-cutting thread (the "Multiplayer at
  Scale" epics A–L in [roadmap.md](../roadmap.md) span multiple phases).

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
(canonical source: [`.github/labels.yml`](https://github.com/fighters-legacy/fighters-legacy/blob/main/.github/labels.yml)):

- **`component: *`** — the subsystem an issue touches (`engine`, `renderer`, `network`,
  `netcode`, `ai`, `flight`, `content`, `server`, `tools`, …). These are **auto-applied to
  PRs** from changed file paths by [`.github/labeler.yml`](https://github.com/fighters-legacy/fighters-legacy/blob/main/.github/labeler.yml), and they
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

**One milestone per phase** — Phase 4 (active) through Phase 9; Phases 1–3 are closed
(Phase 3 gated at `v0.3.0`, 2026-07-10). The milestone answers *when* a piece of work is
scheduled; phase gating (a phase depends on prior phases) is described in
[roadmap.md](../roadmap.md). Assign every issue to its phase milestone at triage. Items with no
scheduled phase get the `backlog` label instead.

**Name milestones by THEME, not by a bare number or letter.** A milestone name carries a
descriptive phrase (`Content & Gameplay`, `Multiplayer at Scale & Live Services`), and the phase
digit is secondary context, never the whole name. A pure `Phase 6` / `Epic C` label reads as
*priority* to anyone who joins later — higher number looks more important — when it only means
*later in the sequence*, and the digits stop mapping to anything once epics are re-homed across
phases (below) or the sequence is re-scoped. The theme is what stays true; lead with it.

**Epics carry the milestone of their *finish* phase** — the phase of their last open
sub-issue. Epics span phases (their sub-issues keep their own per-phase milestones), so an
epic whose decomposition extends into a later phase is re-homed forward rather than left
blocking an earlier milestone or closed with open subs (convention set 2026-07-01 with
#494/#496, applied to #588–#592). The re-home is applied *both ways*: when an epic's last
open sub is pulled *earlier* (e.g. the observer/GM epic #851 finishing inside Phase 4), the
epic re-homes back to that phase. The phase-gate close checklist ([roadmap.md](../roadmap.md))
runs this sweep at every gate.

**Epic decomposition — decompose at phase entry, not at filing.** A newly-created epic is a
*skeleton* (≤ 3 marker sub-issues capturing the known shape); it is fully decomposed into
Feature/Task sub-issues only when its start phase becomes active and it reaches the top of
the Order queue. Decomposing every epic up front is guaranteed rework — scope shifts under
epics that are phases away (the 2026-07-17 review reshaped several before they were ever
picked up). #468 (24 subs) was decomposed because it was next; a Phase 8 epic stays a
skeleton until Phase 8.

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
- **Effort** — *retired 2026-07 (unused).* Options were never defined, no consumer for
  sizing/velocity data ever emerged, and planning runs on release cadence + issue counts.
  Hidden from all three views; not backfilled. (The "define it day one" advice survives in
  the [adoption checklist](#adopting-this-in-a-new-project) for *new* projects — an empty
  field is worse than no field.)
- **Order** — the explicit implementation-order ranking (number field), two layers
  (convention set 2026-07-01, amended 2026-07-17):
  - **Epics: `1–N`** — one unified initiative sequence across all open epics, in planned
    implementation-start order. Derived from the roadmap's dependency records (e.g. the
    scaling spine finishes first; mission runtime → weapons; M precedes N/O/P; H→C→D with
    G alongside and K last). Epics sort to the top of an Order-sorted view as initiative
    headers. **Closed epics vacate their number** — leave the gap; new epics slot into gaps
    at their dependency position. Never renumber.
  - **Work items: phase bands** — the thousands digit encodes the phase sequence, and the
    bands are **permanent**: Phase 4 = `1000s`, Phase 5 = `2000s`, Phase 6 = `3000s`,
    Phase 7 = `4000s`, Phase 8 = `5000s`, Phase 9 = `6000s` (step 10). The **active band is
    simply the lowest band that still has open items** — the original "active phase uses
    small numbers `10+`" rule applied only to the founding phase and retired with the Phase 3
    close, so there is **no renumbering at a phase roll, ever**. An item's band always
    matches its own milestone. Within a band, items group into blocks following the epic
    sequence, and within a block follow the epic's curated sub-issue order; standalones are
    slotted by dependency. Gaps allow insertion without renumbering; re-band items when they
    change milestone.
- **Start Date** / **Target Date** — drive the Roadmap timeline.
- **Parent issue** / **Sub-issues progress** — epic decomposition and rollup.
- **Milestone**, **Labels**, **Assignees** — mirrored from the issue.

## Triage checklist

When opening or grooming an issue, set all of:

- [ ] **Type** — Epic / Feature / Task / Spike / Bug.
- [ ] **Milestone** — the phase it belongs to (or `backlog` label if unscheduled).
- [ ] **`component:` label(s)** — the subsystem(s) it touches.
- [ ] **Project** — confirm it's on the board (auto-add handles new issues).
- [ ] **Parent** — link it under its Epic if it's part of one. *Cross-repo:* every new
      `fl-base-pack` issue is parented under the **active content epic** at triage — **#54**
      (initial content) through the Phase 4 gate, **#1102** (content expansion) after it — so the
      engine board's rollup stays an honest content-readiness gauge. An issue ruled out of the
      gate moves to the successor epic immediately rather than at the gate: while #54 is the
      number being watched, it has to mean what it says.
- [ ] **Status** — `Todo` until picked up.

## Decision records and RFCs

- **Decision records** — during pre-1.0 development, architectural decisions are recorded as
  dated entries in [architecture.md](architecture.md#decision-records), format:
  `**YYYY-MM-DD — <Title> (<Epic>, #<Issue>).** <rationale>`. This is the lightweight,
  high-velocity path for internal architecture.
- **RFCs** — required for public-API, cross-module-architecture, new-major-dependency, and
  community-facing-format changes. Open one with the **RFC issue template**, label it `rfc` +
  `status: under-discussion`, and follow the process in [GOVERNANCE.md](https://github.com/fighters-legacy/fighters-legacy/blob/main/GOVERNANCE.md).

Promote a decision record to a full RFC once the wire protocol / public API freezes (post-1.0).

## Issue → branch → PR → merge

The delivery loop, in brief (full rules in [CONTRIBUTING.md](https://github.com/fighters-legacy/fighters-legacy/blob/main/CONTRIBUTING.md)):

1. **Branch** off `main`: `<type>/<short-kebab-description>`.
2. **Commit** with [Conventional Commits](https://www.conventionalcommits.org/) — the
   `<scope>` mirrors the issue's `component:` label — and a DCO sign-off (`git commit -s`).
3. **PR** referencing the issue (`Closes #NNN`); the title is conventional-commit form
   (enforced by `pr-title-lint`), sign-off is enforced by `dco`, and the
   `component:` labels are applied automatically by `labeler`.
4. **Do not touch `CHANGELOG.md`.** It is generated from commit subjects when a release is cut
   (see below). **The squash-merge subject — which is the PR title — is the published changelog
   line**, so write the PR title for a reader of the release notes.
5. **Merge** once CI is green on all three platforms.

## Cutting a release

**This is the canonical procedure.** `docs/developer/development.md` covers the two scripts; this section
covers the whole thing, including the steps that come *after* the tag — which are the ones that get
skipped.

Releases are `chore(release): vX.Y.Z` PRs, then a tag on the merge commit. The tag fires
`release.yml`, which builds the three platform archives and publishes the GitHub Release.

### The seven steps

1. **Generate the CHANGELOG section.** `./scripts/cut-release.sh vX.Y.Z` creates the release
   branch, runs `scripts/gen_changelog.py` to write a `[X.Y.Z] - YYYY-MM-DD` section from the
   conventional-commit subjects since the last tag, and bumps the CMake
   `project(... VERSION X.Y.Z ...)`. Nothing is hand-maintained: no PR writes into `CHANGELOG.md`,
   so there is nothing to roll (#1123).

   The generator refuses rather than guesses — an empty section, a date that is not today, a
   version already in the file, or **any** change to the released sections below is a hard error.
   `scripts/gen_changelog.py vX.Y.Z --dry-run` prints the section and writes nothing.

2. **Read the generated section as a player would, and check its scope.** Each bullet is a commit
   subject published verbatim, and **this is the last point at which a subject that reads as a
   note-to-self can be fixed** — edit the section in the release PR and say so in the PR body.
   Then verify the scope against the range rather than assuming it:

       git log --oneline <prev-tag>..HEAD

   `cliff.toml` deliberately skips `ci` / `chore` / `build` / `test` / `style`, so a commit missing
   from the section was either correctly skipped or typed with the wrong conventional-commit type.
   Open the release PR and merge it.

3. **Tag the merge commit.** `./scripts/tag-release.sh vX.Y.Z`. The script refuses to tag if the
   `[X.Y.Z]` heading's date is not today — the changelog date, the tag date and the release body
   must all agree, and a release PR that sits overnight otherwise drifts.

4. **Wait for the Release workflow to COMPLETE.** Its `softprops/action-gh-release` step *sets* the
   body to git-cliff's collapsed list. Notes applied before it finishes are silently overwritten.

5. **Verify the archives actually attached.** A green workflow is not evidence:

       gh release view vX.Y.Z --json assets -q '.assets[] | "\(.name) \(.size)"'

   Expect `fighters-legacy-{linux,macos,windows}.zip`, plus `stats.md` and `stats.json`
   (the application statistics, attached to every release — see below).

6. **Hand-author the release body and apply it to this tag.**

       gh release edit vX.Y.Z --notes-file <file>

7. **Read the body back, on the tag you meant.**

       gh release view vX.Y.Z --json body -q '.body' | head -3

8. **Minor releases only (`vX.Y.0`) — confirm the statistics block landed.** It is appended
   automatically *after* the body is hand-authored, by the `Release statistics` workflow, which
   fires on the `release: edited` event step 6 produces.

       gh release view vX.Y.0 --json body -q '.body' | grep -c 'fl-stats:begin'

   Expect `1`. If it is `0`, the workflow has not run yet (give it a minute) or the release has
   no `stats.md` asset — re-run it with `gh workflow run "Release statistics" -f tag=vX.Y.0`.

### Application statistics (milestone gates)

`tools/code_stats.py` reports what the release *is*: composition by category — production code,
test code, documentation, configuration, build system, data, fixtures, media — and the product
surface, meaning wire messages, configuration keys, admin commands, Lua bindings, CLI tools and
test counts.

Both `stats.md` and `stats.json` attach to **every** release, so the JSON forms a comparable
series across versions. The human-readable block is appended to the body of **minor releases
only** (`vX.Y.0`), because a milestone gate is the thing worth measuring and a table on every
patch is noise.

Two properties make it safe to run against a hand-authored body:

- It only ever rewrites text **between its own `<!-- fl-stats:begin/end -->` markers**. Prose
  above them is untouched, and re-running replaces the block rather than stacking copies.
- It **refuses to write into an empty body**, since an empty body means step 6 has not happened
  and the first thing a reader would meet would be a statistics table.

The numbers come from the `stats.md` asset generated at tag time, not from a later checkout — the
statistics have to describe the tagged tree, and re-deriving them from a moving branch would
quietly report something else. The surface counts are imported from `tools/docs_drift.py` rather
than re-implemented, so the release notes and the drift gate cannot disagree about how many
configuration keys the server has.

### What a release body contains

    *Tagged YYYY-MM-DD.*

    **Bold thematic headline.** One to two paragraphs naming the release's major theme(s)
    and the *why* — what changed about the product, not a list of commits.

    ### Added / ### Changed / ### Fixed
    (the categorized detail — paste the [X.Y.Z] CHANGELOG section)

`release.yml` builds its initial body from `cliff.toml`, the same config `gen_changelog.py` uses, so
the workflow's autogenerated body and the `[X.Y.Z]` CHANGELOG section are identical by construction.
What step 6 adds is the prose above them, which no generator can produce.

The leading `*Tagged …*` line states the **git tag date**. A GitHub release object's own date is
not reliable — a release created after the fact reports the day it was created, not the day the
version shipped. The sibling `jomkz/fighters-codex` releases are the house style to match.

A release with no attached archives gets a `### Downloads` section saying so and why. Binaries are
**not** reconstructed later: a zip built today under an old tag uses a different toolchain and
different dependency versions, so it is not what that tag produced.

### Why each step exists

Every one of these was earned by a real defect found in the 2026-07-26 audit of all 18 tags. Two of
the original entries are gone because generation made them impossible rather than because they were
fixed: content cannot sit orphaned under `[Unreleased]` for the next release to sweep up (v0.3.8),
and a change cannot be missing because it closed no issue (#828 in v0.3.2) — the commit is the
source, not the issue.

| Step | Defect it prevents |
|---|---|
| 2 — read the section | v0.3.9 shipped a changelog line written as a note to its author. Under generation the subject is published verbatim, so the release PR is the only place to catch one |
| 2 — verify scope | a commit typed `chore:` or `build:` when it was really a `fix:` is silently absent from the changelog — the one failure mode generation introduces |
| 3 — date guard | `[0.2.0]` and `[0.3.3]` each shipped a day out from their tags |
| 4 — wait for completion | v0.3.8's hand-authored body was overwritten by the workflow |
| 5 — verify artifacts | seven releases published with a green workflow and no archives attached; v0.2.1 attached four Vulkan SDK sample zips instead of the build |
| 6 — hand-author | v0.3.9 shipped git-cliff's six-line PR list and was never touched |
| 7 — read back | v0.3.9's notes were applied to **v0.3.8**, and v0.2.5's to **v0.2.6** — both clobbered a previous release and went unnoticed for months |

### The automated gate

`.github/workflows/release-notes-gate.yml` enforces steps 6 and 7. It checks that a release body
leads with prose rather than a git-cliff heading, and that it carries no section heading for another
version.

Two GitHub behaviours shape when it runs, both confirmed empirically on v0.3.10:

- **Events created by `GITHUB_TOKEN` do not trigger workflows.** `release.yml` publishes with
  `GITHUB_TOKEN`, so `release: published` never fires for our own releases.
- **A `release` event runs the workflow file from the release's *tag*,** not from the default
  branch. Every other trigger runs the default-branch copy.

| Trigger | Runs the copy on | Covers |
|---|---|---|
| `workflow_run` on **Release** | default branch | the publish path — this is what caught v0.3.10 |
| `release: edited` | **the release's tag** | clears the gate when the body is fixed |
| `schedule`, weekly | default branch | every release, forever — **the actual safety net** |

The `release: edited` trigger is convenience, not the guarantee: a release tagged before a fix to
the gate keeps running the old copy of the gate indefinitely. v0.3.10 is permanently in that state,
since it was tagged before the injection fix. The **weekly sweep** is what makes the gate reliable —
it runs current logic over every release and so is immune to both behaviours above. It is also the
only thing that can catch a release that rotted *after* publication, which is how v0.3.8 and v0.3.9
each stayed wrong for months.

Expect the gate to **fail** right after a tag: the body is still git-cliff's at that point. It goes
green once step 6 lands, and closes the tracking issue it opened.

## Adopting this in a new project

A copy-this checklist to stand up the same system in a fresh repo:

1. **Enable issue types** for the org (Settings → Issue types): Epic, Feature, Task, Spike,
   Bug. Do this *before* filing issues so there's no untyped tail.
2. **Create the label set** — start from [`.github/labels.yml`](https://github.com/fighters-legacy/fighters-legacy/blob/main/.github/labels.yml); keep
   the `component: *` taxonomy aligned with your commit scopes.
3. **Wire path-based labeling** — copy [`.github/labeler.yml`](https://github.com/fighters-legacy/fighters-legacy/blob/main/.github/labeler.yml) and
   the `labeler` workflow before the first PR.
4. **Add issue & PR templates** — [`.github/ISSUE_TEMPLATE/`](https://github.com/fighters-legacy/fighters-legacy/tree/main/.github/ISSUE_TEMPLATE)
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

- **Define the `Effort` options at project creation — or delete the field.** The field
  existed here but was never given options, so sizing/velocity tracking never happened; it
  was retired unused on 2026-07 rather than retro-sized across 246 issues. Pick a scale
  (T-shirt `XS`–`XL` or Fibonacci) up front and apply it going forward, or don't create the
  field — an empty field is worse than no field.
- **Decompose epics at phase entry, not at filing.** Epics filed far ahead of their phase
  were reshaped before pickup (the 2026-07-17 review), so any up-front decomposition would
  have been rework. Keep far-off epics as ≤ 3-marker skeletons; decompose when the phase
  activates and the epic tops the Order queue.
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
