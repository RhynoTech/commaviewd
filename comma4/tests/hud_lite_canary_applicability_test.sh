#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

run_ref() {
  local label="$1"
  local checkout="$2"
  local patch="$3"

  echo "=== ${label} ==="
  [[ -e "$checkout/.git" ]] || fail "missing cached checkout $checkout"

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
  git -C "$checkout" apply --check "$patch" || fail "patch does not apply cleanly for ${label}"
  git -C "$checkout" apply "$patch"

  grep -Fq 'commaViewControl' "$checkout/cereal/services.py" || fail "control service missing for ${label}"
  grep -Fq 'commaViewScene' "$checkout/cereal/services.py" || fail "scene service missing for ${label}"
  grep -Fq 'commaViewStatus' "$checkout/cereal/services.py" || fail "status service missing for ${label}"
  grep -Fq 'struct CommaViewControl' "$checkout/cereal/commaview.capnp" || fail "schema missing for ${label}"
  grep -Fq 'commaViewControl @150' "$checkout/cereal/log.capnp" || fail "control event missing for ${label}"
  grep -Fq 'commaViewScene @151' "$checkout/cereal/log.capnp" || fail "scene event missing for ${label}"
  grep -Fq 'commaViewStatus @152' "$checkout/cereal/log.capnp" || fail "status event missing for ${label}"
  grep -Fq '_publish_commaview_control' "$checkout/selfdrive/ui/ui_state.py" || fail "control publisher missing for ${label}"
  grep -Fq '_publish_commaview_scene' "$checkout/selfdrive/ui/ui_state.py" || fail "scene publisher missing for ${label}"
  grep -Fq '_publish_commaview_status' "$checkout/selfdrive/ui/ui_state.py" || fail "status publisher missing for ${label}"

  printf '%s
' '{"healthy":false,"patchVerified":true,"statusScope":"patch-installation","controlServicePresent":true,"sceneServicePresent":true,"statusServicePresent":true,"schemaPresent":true,"controlPublisherPresent":true,"scenePublisherPresent":true,"statusPublisherPresent":true,"controlEventPresent":true,"sceneEventPresent":true,"statusEventPresent":true}'

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

run_ref 'openpilot release-mici-staging' '/home/pear/.cache/ci-ref-checkouts/openpilot-release-mici-staging' "$OPENPILOT_PATCH"
run_ref 'openpilot nightly' '/home/pear/.cache/ci-ref-checkouts/openpilot-nightly' "$OPENPILOT_PATCH"
run_ref 'sunnypilot staging' '/home/pear/.cache/ci-ref-checkouts/sunnypilot-staging' "$SUNNYPILOT_PATCH"
run_ref 'sunnypilot dev' '/home/pear/.cache/ci-ref-checkouts/sunnypilot-dev' "$SUNNYPILOT_PATCH"

echo 'PASS: direct v2 UI export patch applies and verifies on real canary refs'
