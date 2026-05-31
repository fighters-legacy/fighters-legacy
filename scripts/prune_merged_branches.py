#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 John McKenzie <anthropic@mkz.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Prune local and remote branches that have already been merged into main.

Uses the GitHub API (via `gh`) as the primary source so squash-merged and
rebase-merged PR branches are found correctly. Falls back to `git branch
--merged` for any branches not associated with a PR.

Usage:
    python scripts/prune_merged_branches.py [--remote REMOTE] [--base BRANCH] [--dry-run]

Options:
    --remote REMOTE   Remote name to prune (default: origin)
    --base BRANCH     Base branch to compare against (default: main)
    --dry-run         Print what would be deleted without doing it
    --local-only      Skip remote branch deletion
    --remote-only     Skip local branch deletion
"""

import argparse
import json
import subprocess
import sys


PROTECTED = {"main", "master", "develop", "dev", "release", "staging"}


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def fetch_and_prune(remote: str, dry_run: bool) -> None:
    print(f"Fetching {remote} and pruning stale tracking refs...")
    if dry_run:
        print(f"  [dry-run] git fetch --prune {remote}")
        return
    result = run(["git", "fetch", "--prune", remote], check=False)
    if result.returncode != 0:
        print(f"  Warning: fetch failed: {result.stderr.strip()}", file=sys.stderr)


def gh_merged_branch_names() -> set[str] | None:
    """Query GitHub for all merged PR head branch names. Returns None if gh unavailable."""
    result = run(["gh", "pr", "list", "--state", "merged", "--json", "headRefName", "--limit", "1000"], check=False)
    if result.returncode != 0:
        return None
    try:
        return {pr["headRefName"] for pr in json.loads(result.stdout)}
    except (json.JSONDecodeError, KeyError):
        return None


def git_merged_branch_names(base_ref: str, remote: bool, remote_name: str = "") -> set[str]:
    """Fallback: git branch --merged. Misses squash/rebase merges."""
    if remote:
        result = run(["git", "branch", "-r", "--merged", base_ref], check=False)
        prefix = f"{remote_name}/"
        names = set()
        for line in result.stdout.splitlines():
            name = line.strip()
            if name.startswith(prefix):
                names.add(name[len(prefix):])
        return names
    else:
        result = run(["git", "branch", "--merged", base_ref], check=False)
        return {line.strip().lstrip("* ") for line in result.stdout.splitlines() if line.strip()}


def existing_remote_branches(remote: str) -> set[str]:
    result = run(["git", "branch", "-r"], check=False)
    prefix = f"{remote}/"
    names = set()
    for line in result.stdout.splitlines():
        name = line.strip()
        if name.startswith(prefix):
            short = name[len(prefix):]
            if short != "HEAD" and not short.startswith("HEAD ->"):
                names.add(short)
    return names


def existing_local_branches() -> set[str]:
    result = run(["git", "branch"], check=False)
    return {line.strip().lstrip("* ") for line in result.stdout.splitlines() if line.strip()}


def current_branch() -> str:
    return run(["git", "rev-parse", "--abbrev-ref", "HEAD"]).stdout.strip()


def filter_protected(names: set[str]) -> set[str]:
    return {b for b in names if b not in PROTECTED and b != "HEAD" and not b.startswith("HEAD ->")}


def delete_remote_branches(remote: str, branches: list[str], dry_run: bool) -> None:
    if not branches:
        print("No remote branches to delete.")
        return
    print(f"\nRemote branches to delete ({remote}):")
    for b in sorted(branches):
        print(f"  {remote}/{b}")
    if dry_run:
        print(f"\n  [dry-run] would run: git push {remote} --delete <{len(branches)} branches>")
        return
    confirm = input(f"\nDelete {len(branches)} remote branch(es)? [y/N] ").strip().lower()
    if confirm != "y":
        print("Skipping remote deletion.")
        return
    # Batch into chunks of 20 to avoid very long command lines
    chunk_size = 20
    deleted = 0
    branch_list = sorted(branches)
    for i in range(0, len(branch_list), chunk_size):
        chunk = branch_list[i:i + chunk_size]
        result = run(["git", "push", remote, "--delete"] + chunk, check=False)
        if result.returncode != 0:
            print(f"  Error: {result.stderr.strip()}", file=sys.stderr)
        else:
            deleted += len(chunk)
    print(f"  Deleted {deleted} remote branch(es).")


def delete_local_branches(branches: list[str], dry_run: bool) -> None:
    if not branches:
        print("No local branches to delete.")
        return
    print(f"\nLocal branches to delete:")
    for b in sorted(branches):
        print(f"  {b}")
    if dry_run:
        print(f"\n  [dry-run] would run: git branch -D <{len(branches)} branches>")
        return
    confirm = input(f"\nDelete {len(branches)} local branch(es)? [y/N] ").strip().lower()
    if confirm != "y":
        print("Skipping local deletion.")
        return
    for b in sorted(branches):
        # -D instead of -d because squash-merged branches aren't "merged" to git
        result = run(["git", "branch", "-D", b], check=False)
        if result.returncode != 0:
            print(f"  Warning: could not delete '{b}': {result.stderr.strip()}")
        else:
            print(f"  Deleted: {b}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--remote", default="origin", help="Remote name (default: origin)")
    parser.add_argument("--base", default="main", help="Base branch (default: main)")
    parser.add_argument("--dry-run", action="store_true", help="Print actions without executing")
    parser.add_argument("--local-only", action="store_true", help="Skip remote branch deletion")
    parser.add_argument("--remote-only", action="store_true", help="Skip local branch deletion")
    args = parser.parse_args()

    result = run(["git", "rev-parse", "--git-dir"], check=False)
    if result.returncode != 0:
        print("Error: not inside a git repository.", file=sys.stderr)
        sys.exit(1)

    fetch_and_prune(args.remote, args.dry_run)

    base_ref = f"{args.remote}/{args.base}"

    # Primary source: GitHub API (handles squash + rebase merges)
    gh_names = gh_merged_branch_names()
    if gh_names is not None:
        print(f"  GitHub API: found {len(gh_names)} merged PR branch name(s).")
        merged_names = filter_protected(gh_names)
    else:
        print("  gh not available or not authenticated — falling back to git branch --merged.")
        merged_names = filter_protected(git_merged_branch_names(base_ref, remote=False))

    # Supplement with git --merged to catch branches not associated with a PR
    git_merged = filter_protected(git_merged_branch_names(base_ref, remote=False))
    merged_names = merged_names | git_merged

    if not args.local_only:
        remote_existing = existing_remote_branches(args.remote)
        to_delete_remote = sorted(merged_names & remote_existing)
        delete_remote_branches(args.remote, to_delete_remote, args.dry_run)

    if not args.remote_only:
        local_existing = existing_local_branches()
        cur = current_branch()
        to_delete_local = sorted((merged_names & local_existing) - {cur})
        delete_local_branches(to_delete_local, args.dry_run)

    print("\nDone.")


if __name__ == "__main__":
    main()
