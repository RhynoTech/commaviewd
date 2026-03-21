#!/usr/bin/env bash
set -euo pipefail

MANIFEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android-schema/manifest.json"
CONTRACT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android-schema/contract-manifest.json"
IGNORE_MANIFEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android-schema/ignore-manifest.json"
UPSTREAM_ROOT=""
MODE="fail"
LABEL="upstream"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest)
      MANIFEST="$2"; shift 2 ;;
    --contract)
      CONTRACT="$2"; shift 2 ;;
    --ignore-manifest)
      IGNORE_MANIFEST="$2"; shift 2 ;;
    --upstream-root)
      UPSTREAM_ROOT="$2"; shift 2 ;;
    --label)
      LABEL="$2"; shift 2 ;;
    --mode)
      MODE="$2"; shift 2 ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2 ;;
  esac
done

if [[ -z "$UPSTREAM_ROOT" ]]; then
  echo "Usage: $0 --upstream-root <path> [--manifest <path>] [--contract <path>] [--ignore-manifest <path>] [--label <label>] [--mode fail|warn|report|suggest]" >&2
  exit 2
fi

[[ -f "$CONTRACT" ]] || { echo "FAIL: missing contract manifest $CONTRACT" >&2; exit 2; }
[[ -f "$IGNORE_MANIFEST" ]] || { echo "FAIL: missing ignore manifest $IGNORE_MANIFEST" >&2; exit 2; }
[[ -d "$UPSTREAM_ROOT" ]] || { echo "FAIL: missing upstream root $UPSTREAM_ROOT" >&2; exit 2; }
mkdir -p dist

python3 scripts/check_android_schema_drift.py \
  --manifest "$MANIFEST" \
  --contract "$CONTRACT" \
  --ignore-manifest "$IGNORE_MANIFEST" \
  --upstream-root "$UPSTREAM_ROOT" \
  --label "$LABEL" \
  --mode "$MODE"
