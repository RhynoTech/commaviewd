#!/usr/bin/env bash
# Runs commaviewd-ci's checks on this machine, so CI only has to confirm them before a merge.
#
#   scripts/ci-local.sh                      # the source releases build from (sunnypilot-release-pin)
#   scripts/ci-local.sh openpilot-release-mici sunnypilot-release-tizi
#   scripts/ci-local.sh --all                # every CI target (ci/targets.json)
#   scripts/ci-local.sh --list               # the targets
#   scripts/ci-local.sh --targets ci/canary-sunnypilot.json --all   # a canary's targets
#
# Each target, as .github/workflows/commaviewd-verify.yml does it (ci/plan-targets.py): resolve
# the upstream branch, check out its source (blobless and without the driving models, kept under
# $CI_LOCAL_SRC_DIR for the next run), run the interface guard and check the onroad UI export
# patch applies; then, once per upstream commit, run commaviewd/scripts/run-verification.sh; then
# the telemetry guard.
# The verification build needs the cross toolchain from scripts/install-commaviewd-toolchain.sh
# (Ubuntu 24.04; it rewrites the apt sources, so run it in a container or VM, not on a laptop).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${CI_LOCAL_SRC_DIR:-$HOME/.cache/commaviewd-ci-src}"
cd "$REPO_ROOT"

names=()
targets_file=ci/targets.json
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --targets) targets_file="${2:?--targets needs a file}"; shift ;;
    --list) list=1 ;;
    --all) all=1 ;;
    -*) echo "Unknown option: $1 (try --help)" >&2; exit 2 ;;
    *) names+=("$1") ;;
  esac
  shift
done
if [[ -n "${list:-}" ]]; then
  python3 -c 'import json, sys; [print(t["name"]) for t in json.load(open(sys.argv[1]))]' "$targets_file"
  exit 0
fi
if [[ -z "${all:-}" && ${#names[@]} -eq 0 ]]; then
  [[ "$targets_file" == ci/targets.json ]] || { echo "Name targets (--list) or pass --all" >&2; exit 2; }
  names=(sunnypilot-release-pin)
fi

step() { printf '\n==> %s\n' "$*"; }

step "Planning targets"
plan="$(ci/plan-targets.py --targets "$targets_file" "${names[@]}")"
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

# The same tree CI checks out, blobless, sparse without the driving models, submodules included.
# Prints where it is.
checkout_source() {
  local repo=$1 sha=$2 dir=$3
  if [[ -f "$dir/.ci-local-complete" ]]; then
    git -C "$dir" reset --hard -q HEAD
    git -C "$dir" clean -fdq
    echo "$dir"
    return 0
  fi
  # Check out beside it and move it in once complete, so an interrupted run leaves no half tree
  # where the next run would take it for a finished one.
  local partial
  mkdir -p "$(dirname "$dir")"
  partial="$(mktemp -d "$dir.partial-XXXXXX")"
  git -C "$partial" init -q
  git -C "$partial" remote add origin "https://github.com/${repo}.git"
  git -C "$partial" sparse-checkout set --no-cone '/*' '!/selfdrive/modeld/models/' '!/openpilot/selfdrive/modeld/models/'
  git -C "$partial" fetch -q --depth 1 --filter=blob:none origin "$sha"
  git -C "$partial" checkout -q FETCH_HEAD
  git -C "$partial" submodule update -q --init --recursive --depth 1
  touch "$partial/.ci-local-complete"
  if [[ -e "$dir" ]]; then
    echo "$dir exists but isn't a complete checkout: using $partial (remove $dir to tidy up)" >&2
    dir="$partial"
  else
    mv "$partial" "$dir"
  fi
  echo "$dir"
}

failed=()
while IFS=$'\t' read -r name repo platform sha run_verification why; do
  step "$name ($repo $sha, $platform)"
  echo "upstream checkout: $SRC_DIR/${repo//\//-}/$sha"
  # Not `if ! ( ... )`: bash ignores set -e inside a subshell that is a condition.
  set +e
  (
    set -euo pipefail
    op_root="$(checkout_source "$repo" "$sha" "$SRC_DIR/${repo//\//-}/$sha")"
    mkdir -p dist
    OP_ROOT="$op_root" ./commaviewd/scripts/upstream-interface-guard.sh --telemetry-only
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
