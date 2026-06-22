#!/usr/bin/env bash
# Roadmap progress report against GitHub issue milestones.
# Requires: gh (authenticated), jq, GNU date (Linux / macOS with coreutils).
#
# Each "Phase N" milestone carries its own open/closed issue counts and a due
# date. Milestones have no start date, so the % elapsed / on-track signal uses a
# sequential window per phase: a phase is assumed to start when the previous
# (earlier-due) phase is due, and the first phase starts at its creation date.

set -euo pipefail

OWNER="fighters-legacy"
REPO="fighters-legacy"
TODAY=$(date +%Y-%m-%d)
TODAY_TS=$(date -d "$TODAY" +%s)

# title, due (date only or ""), open, closed, state, created (date only)
# sorted by due date ascending; milestones with no due date sort last.
# Fields joined with the ASCII Unit Separator (0x1F): a non-whitespace delimiter
# so `read` preserves empty fields (a milestone with no due date) instead of
# collapsing consecutive tabs and shifting every later column.
MILESTONES=$(gh api "repos/$OWNER/$REPO/milestones?state=all&per_page=100" --jq '
  sort_by(.due_on // "9999-12-31T00:00:00Z")[] |
  [ .title,
    (.due_on // "" | sub("T.*"; "")),
    .open_issues,
    .closed_issues,
    .state,
    (.created_at | sub("T.*"; "")) ] | map(tostring) | join("\u001f")
')

RESET=$'\e[0m'
GREEN=$'\e[32m'
YELLOW=$'\e[33m'
RED=$'\e[31m'
GRAY=$'\e[37m'

printf "\nFighters Legacy — Roadmap Status (%s)\n" "$TODAY"
printf '═%.0s' {1..62}
printf "\n\n"

prev_due=""
while IFS=$'\037' read -r title due open closed state created; do
  total=$(( open + closed ))
  if [[ $total -eq 0 ]]; then
    prev_due="$due"
    continue
  fi

  pct_done=$(( closed * 100 / total ))

  # Split "Phase N — Description" on the em dash for two-column layout.
  prefix="${title%% — *}"
  desc="${title#* — }"
  [[ "$desc" == "$title" ]] && desc=""

  status="OPEN"; pct_elapsed=0; days_str=""; clr="$GRAY"

  if [[ "$state" == "closed" || $pct_done -ge 100 ]]; then
    status="COMPLETE"; pct_elapsed=100; clr="$GREEN"
    [[ -n "$due" ]] && days_str="due $due"
  elif [[ -z "$due" ]]; then
    status="UNSCHEDULED"; days_str="no due date"; clr="$GRAY"
  else
    due_ts=$(date -d "$due" +%s)
    days_remain=$(( (due_ts - TODAY_TS) / 86400 ))
    # Phase window start = previous milestone's due date (sequential roadmap),
    # falling back to this milestone's creation date for the first phase.
    start="${prev_due:-$created}"
    start_ts=$(date -d "$start" +%s)

    if [[ $TODAY_TS -gt $due_ts ]]; then
      pct_elapsed=100; status="OVERDUE"; clr="$RED"
      days_str="${days_remain#-}d overdue"
    elif [[ $TODAY_TS -lt $start_ts ]]; then
      status="NOT STARTED"; pct_elapsed=0; clr="$GRAY"
      days_str="starts in $(( (start_ts - TODAY_TS) / 86400 ))d"
    else
      total_days=$(( (due_ts - start_ts) / 86400 ))
      [[ $total_days -le 0 ]] && total_days=1
      elapsed_days=$(( (TODAY_TS - start_ts) / 86400 ))
      pct_elapsed=$(( elapsed_days * 100 / total_days ))
      threshold_ok=$(( pct_elapsed * 80 / 100 ))
      threshold_risk=$(( pct_elapsed * 50 / 100 ))
      if   [[ $pct_done -ge $threshold_ok   ]]; then status="ON TRACK"; clr="$GREEN"
      elif [[ $pct_done -ge $threshold_risk ]]; then status="AT RISK";  clr="$YELLOW"
      else                                           status="BEHIND";   clr="$RED"
      fi
      days_str="${days_remain}d remain"
    fi
  fi

  filled=$(( pct_done * 20 / 100 ))
  bar=""; for ((i=0; i<20; i++)); do [[ $i -lt $filled ]] && bar+="█" || bar+="░"; done

  case "$status" in
    "ON TRACK"|"COMPLETE")              icon="✓" ;;
    "NOT STARTED"|"UNSCHEDULED"|"OPEN") icon="·" ;;
    "AT RISK")                          icon="⚠" ;;
    *)                                  icon="✗" ;;
  esac

  printf "${clr}%-10s %-36s${RESET}  %s\n" "$prefix" "$desc" "$days_str"
  printf "           ${clr}%s${RESET}  %3d%% done  %3d%% elapsed  %s %s\n" \
    "$bar" "$pct_done" "$pct_elapsed" "$icon" "$status"
  printf "           %d / %d issues closed\n\n" "$closed" "$total"

  prev_due="$due"
done <<< "$MILESTONES"
