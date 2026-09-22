#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: comma4-bench-switch.sh baseline|old|new [bench-repo]

Run this on an offroad comma device. The default bench repo is
/data/commaview-bench-src. It switches only the installed UI-export companion
files; it deliberately keeps the installed commaviewd binary fixed.
EOF
}

mode="${1:-}"
bench_repo="${2:-/data/commaview-bench-src}"
install_dir="/data/commaview"
op_root="/data/openpilot"
old_ref="211269e"
new_ref="1cc9a7e"

case "$mode" in
  baseline|old|new) ;;
  -h|--help|'') usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

[[ -d "$op_root/.git" ]] || { echo "ERROR: $op_root is not a git checkout" >&2; exit 1; }
[[ -x "$install_dir/stop.sh" ]] || { echo "ERROR: CommaView is not installed at $install_dir" >&2; exit 1; }

is_onroad="$(tr -d '\000\r\n' < /data/params/d/IsOnroad 2>/dev/null || true)"
[[ "$is_onroad" != "1" ]] || { echo "ERROR: device is onroad; refusing bench switch" >&2; exit 42; }

for session in cv-replay cv-ui cv-metrics; do
  tmux kill-session -t "$session" 2>/dev/null || true
done

if [[ "$mode" == "baseline" ]]; then
  if [[ -x "$install_dir/scripts/revert_onroad_ui_export_patch.sh" ]]; then
    "$install_dir/scripts/revert_onroad_ui_export_patch.sh"
  fi
  "$install_dir/stop.sh"
  if pgrep -f '/data/commaview/commaviewd (bridge|control)' >/dev/null 2>&1; then
    echo "ERROR: a commaviewd process is still running" >&2
    exit 1
  fi
  if find "$op_root" -path '*/selfdrive/ui/commaview_export.py' -type f -print -quit | grep -q .; then
    echo "ERROR: CommaView UI exporter is still installed" >&2
    exit 1
  fi
  mkdir -p /data/commaview-bench/results
  {
    printf 'label=A baseline\n'
    printf 'sourceRef=none\n'
    printf 'openpilotRef=%s\n' "$(git -C "$op_root" rev-parse HEAD)"
    printf 'helper=absent\n'
    printf 'workerMarker=absent\n'
    printf 'commaviewd=stopped\n'
  } | tee /data/commaview-bench/results/baseline-identity.txt
  echo "READY: A baseline; CommaView stopped and UI exporter absent"
  exit 0
fi

[[ -d "$bench_repo/.git" ]] || { echo "ERROR: bench repo missing at $bench_repo" >&2; exit 1; }
ref="$old_ref"
expected_worker="absent"
label="B old synchronous exporter"
if [[ "$mode" == "new" ]]; then
  ref="$new_ref"
  expected_worker="present"
  label="C new isolated-worker exporter"
fi

git -C "$bench_repo" cat-file -e "${ref}^{commit}"
git -C "$bench_repo" checkout --detach --quiet "$ref"
resolved_ref="$(git -C "$bench_repo" rev-parse HEAD)"

"$install_dir/stop.sh"
mkdir -p "$install_dir/scripts" "$install_dir/src"
cp -a "$bench_repo/comma/scripts/." "$install_dir/scripts/"
cp -a "$bench_repo/comma/src/." "$install_dir/src/"

COMMAVIEWD_INSTALL_DIR="$install_dir" \
COMMAVIEWD_OP_ROOT="$op_root" \
  "$install_dir/scripts/apply_onroad_ui_export_patch.sh" --force-repair --platform mici

COMMAVIEWD_RESTART_REASON="comma4-bench-${mode}" "$install_dir/start.sh"

helper="$(find "$op_root" -path '*/selfdrive/ui/commaview_export.py' -type f -print -quit)"
[[ -n "$helper" ]] || { echo "ERROR: installed UI exporter was not found" >&2; exit 1; }
worker_marker="absent"
grep -Fq 'def _worker_main(self) -> None:' "$helper" && worker_marker="present"
[[ "$worker_marker" == "$expected_worker" ]] || {
  echo "ERROR: worker marker is $worker_marker, expected $expected_worker" >&2
  exit 1
}

mkdir -p /data/commaview-bench/results
{
  printf 'label=%s\n' "$label"
  printf 'sourceRef=%s\n' "$resolved_ref"
  printf 'openpilotRef=%s\n' "$(git -C "$op_root" rev-parse HEAD)"
  printf 'helper=%s\n' "$helper"
  printf 'workerMarker=%s\n' "$worker_marker"
  sha256sum "$helper"
} | tee "/data/commaview-bench/results/${mode}-identity.txt"

echo "READY: $label at $resolved_ref"
