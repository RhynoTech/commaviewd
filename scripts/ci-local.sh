#!/usr/bin/env bash
# Runs commaviewd-ci's checks on this machine, so CI only has to confirm them before a merge.
#
#   scripts/ci-local.sh                      # the source releases build from (sunnypilot-release-pin)
#   scripts/ci-local.sh openpilot-release-mici sunnypilot-release-tizi
#   scripts/ci-local.sh --all                # every CI target (ci/targets.json)
#   scripts/ci-local.sh --list               # the targets
#
# Each target, as CI does it (ci/plan-targets.py): resolve the upstream branch, check out its
# source (blobless and without the driving models, kept under $CI_LOCAL_SRC_DIR for the next run)
# and check the onroad UI export patch applies; then, once per upstream commit, run
# commaviewd/scripts/run-verification.sh; then the telemetry guard.
# The verification build needs the cross toolchain from scripts/install-commaviewd-toolchain.sh
# (Ubuntu 24.04; it rewrites the apt sources, so run it in a container or VM, not on a laptop).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${CI_LOCAL_SRC_DIR:-$HOME/.cache/commaviewd-ci-src}"
cd "$REPO_ROOT"

names=()
for arg in "$@"; do
  case "$arg" in
    -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --list) python3 -c 'import json; [print(t["name"]) for t in json.load(open("ci/targets.json"))]'; exit 0 ;;
    --all) all=1 ;;
    -*) echo "Unknown option: $arg (try --help)" >&2; exit 2 ;;
    *) names+=("$arg") ;;
  esac
done
if [[ -z "${all:-}" && ${#names[@]} -eq 0 ]]; then
  names=(sunnypilot-release-pin)
fi

step() { printf '\n==> %s\n' "$*"; }

step "Planning targets"
plan="$(ci/plan-targets.py "${names[@]}")"
# name repo ui_platform sha run_verification why, one target per line
targets="$(python3 -c '
import json, sys
for t in json.loads(sys.argv[1]):
    print("\t".join([t["name"], t["upstream_repo"], t["ui_platform"], t["sha"],
                     str(t["run_verification"]).lower(), t.get("why", "")]))
' "$plan")"
echo "$targets" | cut -f1,4,5

if grep -q "	true	" <<<"$targets"; then
  ARM_CAPNP_SO="${ARM_CAPNP_SO:-$(ls -1 /usr/lib/aarch64-linux-gnu/libcapnp-*.so 2>/dev/null | head -n1 || true)}"
  ARM_KJ_SO="${ARM_KJ_SO:-$(ls -1 /usr/lib/aarch64-linux-gnu/libkj-*.so 2>/dev/null | head -n1 || true)}"
  if ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 || [[ -z "$ARM_CAPNP_SO" || -z "$ARM_KJ_SO" ]]; then
    echo "The arm64 cross toolchain isn't installed: scripts/install-commaviewd-toolchain.sh" >&2
    exit 1
  fi
  export ARM_CAPNP_SO ARM_KJ_SO
fi

# The same tree CI checks out: blobless, sparse without the driving models, submodules included.
checkout_source() {
  local repo=$1 sha=$2 dir=$3
  if [[ -f "$dir/.ci-local-complete" ]]; then
    git -C "$dir" reset --hard -q HEAD
    git -C "$dir" clean -fdq
    return 0
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" remote add origin "https://github.com/${repo}.git"
  git -C "$dir" sparse-checkout set --no-cone '/*' '!/selfdrive/modeld/models/' '!/openpilot/selfdrive/modeld/models/'
  git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$sha"
  git -C "$dir" checkout -q FETCH_HEAD
  git -C "$dir" submodule update -q --init --recursive --depth 1
  touch "$dir/.ci-local-complete"
}

failed=()
while IFS=$'\t' read -r name repo platform sha run_verification why; do
  step "$name ($repo $sha, $platform)"
  op_root="$SRC_DIR/${repo//\//-}/$sha"
  # Not `if ! ( ... )`: bash ignores set -e inside a subshell that is a condition.
  set +e
  (
    set -euo pipefail
    checkout_source "$repo" "$sha" "$op_root"
    mkdir -p dist
    COMMAVIEWD_INSTALL_DIR="$REPO_ROOT/comma" COMMAVIEWD_OP_ROOT="$op_root" ./comma/scripts/apply_onroad_ui_export_patch.sh --platform "$platform"
    COMMAVIEWD_INSTALL_DIR="$REPO_ROOT/comma" COMMAVIEWD_OP_ROOT="$op_root" ./comma/scripts/verify_onroad_ui_export_patch.sh --json --platform "$platform" >/dev/null
    git -C "$op_root" reset --hard -q HEAD
    git -C "$op_root" clean -fdq
    if [[ "$run_verification" == "true" ]]; then
      OP_ROOT="$op_root" RELEASE_SMOKE_TAG="local-$name" commaviewd/scripts/run-verification.sh
    else
      echo "Verification build skipped ($why): patch applicability only"
    fi
    ./scripts/verify-telemetry-hardening.sh
  )
  rc=$?
  set -e
  [[ $rc -eq 0 ]] || failed+=("$name")
done <<<"$targets"

echo
if [[ ${#failed[@]} -gt 0 ]]; then
  echo "FAILED: ${failed[*]}" >&2
  exit 1
fi
echo "commaviewd-ci checks passed"
