#!/usr/bin/env bash
# Roadmap progress report against project date targets.
# Requires: gh (authenticated), jq, GNU date (Linux / macOS with coreutils).

set -euo pipefail

OWNER="jomkz"
PROJECT_NUM=2
TODAY=$(date +%Y-%m-%d)
TODAY_TS=$(date -d "$TODAY" +%s)

DATA=$(gh api graphql -f query='
{
  user(login: "jomkz") {
    projectV2(number: 2) {
      items(first: 100) {
        nodes {
          content {
            ... on Issue { state }
          }
          fieldValues(first: 20) {
            nodes {
              ... on ProjectV2ItemFieldSingleSelectValue {
                name
                field { ... on ProjectV2SingleSelectField { name } }
              }
              ... on ProjectV2ItemFieldDateValue {
                date
                field { ... on ProjectV2Field { name } }
              }
            }
          }
        }
      }
    }
  }
}')

ITEMS=$(echo "$DATA" | jq -r '
  .data.user.projectV2.items.nodes[] |
  {
    state: (.content.state // ""),
    phase: ([.fieldValues.nodes[] | select(.field?.name == "Phase") | .name] | first // ""),
    start: ([.fieldValues.nodes[] | select(.field?.name == "Start Date") | .date] | first // ""),
    end:   ([.fieldValues.nodes[] | select(.field?.name == "Target Date") | .date] | first // "")
  } | [.state, .phase, .start, .end] | @tsv
')

declare -A PHASE_NAME=(
  ["Phase 1"]="Engine Foundation"
  ["Phase 2"]="Modern Particles Engine"
  ["Phase 3"]="Classic/Parity Mode"
  ["Phase 4"]="In-Game Mission Editor"
  ["Phase 5"]="Linux/Mac Release"
  ["Phase 6"]="Native Formats + Free Pack"
)

printf "\nFighters Legacy — Roadmap Status (%s)\n" "$TODAY"
printf '═%.0s' {1..62}
printf "\n\n"

for phase in "Phase 1" "Phase 2" "Phase 3" "Phase 4" "Phase 5" "Phase 6"; do
  total=0; done_count=0; start_date=""; end_date=""

  while IFS=$'\t' read -r state p start end; do
    [[ "$p" != "$phase" ]] && continue
    total=$(( total + 1 ))
    [[ "$state" == "CLOSED" ]] && done_count=$(( done_count + 1 ))
    [[ -z "$start_date" && -n "$start" ]] && start_date="$start"
    [[ -z "$end_date"   && -n "$end"   ]] && end_date="$end"
  done <<< "$ITEMS"

  [[ $total -eq 0 ]] && continue

  pct_done=$(( done_count * 100 / total ))
  status="NOT STARTED"; pct_elapsed=0; days_str=""

  if [[ -n "$start_date" && -n "$end_date" ]]; then
    start_ts=$(date -d "$start_date" +%s)
    end_ts=$(date -d "$end_date" +%s)
    total_days=$(( (end_ts - start_ts) / 86400 ))
    days_remain=$(( (end_ts - TODAY_TS) / 86400 ))

    if   [[ $TODAY_TS -lt $start_ts ]]; then
      status="NOT STARTED"; pct_elapsed=0
      days_str="starts in $(( (start_ts - TODAY_TS) / 86400 ))d"
    elif [[ $TODAY_TS -gt $end_ts ]]; then
      pct_elapsed=100
      [[ $pct_done -ge 100 ]] && status="COMPLETE" || status="OVERDUE"
      days_str="${days_remain#-}d overdue"
    else
      elapsed_days=$(( (TODAY_TS - start_ts) / 86400 ))
      pct_elapsed=$(( elapsed_days * 100 / total_days ))
      threshold_ok=$(( pct_elapsed * 80 / 100 ))
      threshold_risk=$(( pct_elapsed * 50 / 100 ))
      if   [[ $pct_done -ge $threshold_ok   ]]; then status="ON TRACK"
      elif [[ $pct_done -ge $threshold_risk ]]; then status="AT RISK"
      else                                           status="BEHIND"
      fi
      days_str="${days_remain}d remain"
    fi
  fi

  filled=$(( pct_done * 20 / 100 ))
  bar=""; for ((i=0; i<20; i++)); do [[ $i -lt $filled ]] && bar+="█" || bar+="░"; done

  case "$status" in
    "ON TRACK"|"COMPLETE") icon="✓" ;;
    "NOT STARTED")         icon="·" ;;
    "AT RISK")             icon="⚠" ;;
    *)                     icon="✗" ;;
  esac

  printf "%-8s %-30s  ends %s  %s\n" \
    "$phase" "${PHASE_NAME[$phase]:-$phase}" "$end_date" "$days_str"
  printf "         %s  %3d%% done  %3d%% elapsed  %s %s\n" \
    "$bar" "$pct_done" "$pct_elapsed" "$icon" "$status"
  printf "         %d / %d issues closed\n\n" "$done_count" "$total"
done

backlog=0
while IFS=$'\t' read -r state p _start _end; do
  [[ -z "$p" ]] && backlog=$(( backlog + 1 ))
done <<< "$ITEMS"
[[ $backlog -gt 0 ]] && printf "Backlog  %d unscheduled issues\n\n" "$backlog"
