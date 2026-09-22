#!/usr/bin/env bash
set -euo pipefail

label="${1:-}"
case "$label" in
  ''|*[!A-Za-z0-9._-]*) echo "Usage: comma4-bench-snapshot.sh <safe-label>" >&2; exit 2 ;;
esac

root="/data/commaview-bench/results"
dest="$root/$label"
mkdir -p "$dest"

copy_if_present() {
  local source="$1"
  if [[ -e "$source" ]]; then
    cp -a "$source" "$dest/"
  fi
  return 0
}

for source in \
  /data/commaview/logs/commaviewd-bridge.log \
  /data/commaview/logs/commaviewd-control.log \
  /data/commaview/logs/onroad-ui-export-startup.log \
  /data/commaview/logs/runtime-run-events.jsonl \
  /data/commaview/run/telemetry-stats.json \
  /data/commaview/run/runtime-debug-effective.json \
  /data/commaview/run/onroad-ui-export-status.json \
  /data/commaview/run/last-restart-reason.txt; do
  copy_if_present "$source"
done

for source in "$root/${label}-"*; do
  copy_if_present "$source"
done

for session in cv-replay cv-ui cv-metrics comma; do
  tmux capture-pane -p -S -2000 -t "$session" > "$dest/tmux-${session}.log" 2>/dev/null || true
done

{
  date -Ins
  uname -a
  git -C /data/openpilot rev-parse HEAD
  git -C /data/openpilot status --short -- \
    selfdrive/ui openpilot/selfdrive/ui 2>/dev/null || true
  ps -eo pid,stat,etimes,pcpu,pmem,args | grep -E '[r]eplay|[u]i.py|[/]data/commaview/commaviewd' || true
  find /data/openpilot -path '*/selfdrive/ui/commaview_export.py' -type f -exec sha256sum {} \;
} > "$dest/device-state.txt" 2>&1

tar -C "$root" -czf "$root/${label}.tar.gz" "$label"
sha256sum "$root/${label}.tar.gz" | tee "$root/${label}.tar.gz.sha256"
echo "Evidence: $root/${label}.tar.gz"
