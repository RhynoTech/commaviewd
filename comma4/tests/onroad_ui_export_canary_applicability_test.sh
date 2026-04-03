#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"
CACHE_ROOT="${COMMAVIEWD_CANARY_CACHE_ROOT:-$HOME/.cache/ci-ref-checkouts}"
OPENPILOT_REPO="${COMMAVIEWD_OPENPILOT_REPO:-https://github.com/commaai/openpilot.git}"
SUNNYPILOT_REPO="${COMMAVIEWD_SUNNYPILOT_REPO:-https://github.com/sunnyhaibin/sunnypilot.git}"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

run_ref() {
  local label="$1"
  local repo="$2"
  local ref="$3"
  local checkout="$4"
  local patch="$5"
  local expected_runtime_flavor="$6"

  echo "=== ${label} ==="
  mkdir -p "$(dirname "$checkout")"
  if [[ -e "$checkout/.git" ]]; then
    git -C "$checkout" fetch --depth 1 origin "$ref"
    git -C "$checkout" checkout -q FETCH_HEAD
  else
    git clone --depth 1 --branch "$ref" "$repo" "$checkout"
  fi

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
  git -C "$checkout" apply --recount --check "$patch" || fail "patch does not apply cleanly for ${label}"
  git -C "$checkout" apply --recount "$patch"

  grep -Fq 'commaViewControl' "$checkout/cereal/services.py" || fail "control service missing for ${label}"
  grep -Fq 'commaViewScene' "$checkout/cereal/services.py" || fail "scene service missing for ${label}"
  grep -Fq 'commaViewStatus' "$checkout/cereal/services.py" || fail "status service missing for ${label}"
  grep -Fq 'struct CommaViewControl' "$checkout/cereal/commaview.capnp" || fail "schema missing for ${label}"
  grep -Fq 'latActive @14 :Bool;' "$checkout/cereal/commaview.capnp" || fail "latActive field missing for ${label}"
  grep -Fq 'longActive @15 :Bool;' "$checkout/cereal/commaview.capnp" || fail "longActive field missing for ${label}"
  grep -Fq 'experimentalMode @18 :Bool;' "$checkout/cereal/commaview.capnp" || fail "experimentalMode field missing for ${label}"
  grep -Fq 'runtimeFlavor @19 :Text;' "$checkout/cereal/commaview.capnp" || fail "runtimeFlavor field missing for ${label}"
  grep -Fq 'enum CommaViewStatusMode {' "$checkout/cereal/commaview.capnp" || fail "status mode enum missing for ${label}"
  grep -Fq 'enum CommaViewSpeedLimitPreActiveIcon {' "$checkout/cereal/commaview.capnp" || fail "speed-limit icon enum missing for ${label}"
  grep -Fq 'statusMode @20 :CommaViewStatusMode;' "$checkout/cereal/commaview.capnp" || fail "statusMode field missing for ${label}"
  grep -Fq 'speedLimitPreActive @21 :Bool;' "$checkout/cereal/commaview.capnp" || fail "speed-limit pre-active field missing for ${label}"
  grep -Fq 'speedLimitPreActiveIcon @22 :CommaViewSpeedLimitPreActiveIcon;' "$checkout/cereal/commaview.capnp" || fail "speed-limit icon field missing for ${label}"
  grep -Fq 'blindspotIndicatorsEnabled @23 :Bool;' "$checkout/cereal/commaview.capnp" || fail "blindspotIndicatorsEnabled field missing for ${label}"
  grep -Fq 'rainbowPathEnabled @24 :Bool;' "$checkout/cereal/commaview.capnp" || fail "rainbowPathEnabled field missing for ${label}"
  grep -Fq 'commaViewControl @150' "$checkout/cereal/log.capnp" || fail "control event missing for ${label}"
  grep -Fq 'commaViewScene @151' "$checkout/cereal/log.capnp" || fail "scene event missing for ${label}"
  grep -Fq 'commaViewStatus @152' "$checkout/cereal/log.capnp" || fail "status event missing for ${label}"
  grep -Fq '_publish_commaview_control' "$checkout/selfdrive/ui/ui_state.py" || fail "control publisher missing for ${label}"
  grep -Fq '_publish_commaview_scene' "$checkout/selfdrive/ui/ui_state.py" || fail "scene publisher missing for ${label}"
  grep -Fq '_publish_commaview_status' "$checkout/selfdrive/ui/ui_state.py" || fail "status publisher missing for ${label}"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$checkout/selfdrive/ui/ui_state.py" || fail "runtime flavor constant missing for ${label}"
  grep -Fq 'control.latActive = bool(car_control.latActive)' "$checkout/selfdrive/ui/ui_state.py" || fail "latActive export missing for ${label}"
  grep -Fq 'control.longActive = bool(car_control.longActive)' "$checkout/selfdrive/ui/ui_state.py" || fail "longActive export missing for ${label}"
  grep -Fq 'status.runtimeFlavor = COMMAVIEW_RUNTIME_FLAVOR if COMMAVIEW_RUNTIME_FLAVOR in ("OPENPILOT", "SUNNYPILOT") else COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN' "$checkout/selfdrive/ui/ui_state.py" || fail "runtime flavor export missing for ${label}"
  grep -Fq 'status.experimentalMode = bool(selfdrive_state.experimentalMode)' "$checkout/selfdrive/ui/ui_state.py" || fail "experimentalMode export missing for ${label}"
  grep -Fq 'status.blindspotIndicatorsEnabled = bool(self.params.get_bool("BlindSpot"))' "$checkout/selfdrive/ui/ui_state.py" || fail "BlindSpot param export missing for ${label}"
  grep -Fq 'status.rainbowPathEnabled = bool(self.params.get_bool("RainbowMode"))' "$checkout/selfdrive/ui/ui_state.py" || fail "RainbowMode param export missing for ${label}"
  grep -Fq 'def _commaview_status_mode_name(status) -> str:' "$checkout/selfdrive/ui/ui_state.py" || fail "status mode helper missing for ${label}"
  grep -Fq 'status.statusMode = self._commaview_status_mode_name(self.status)' "$checkout/selfdrive/ui/ui_state.py" || fail "status mode export missing for ${label}"
  grep -Fq 'status.speedLimitPreActive = False' "$checkout/selfdrive/ui/ui_state.py" || fail "speed-limit pre-active default missing for ${label}"
  grep -Fq 'status.speedLimitPreActiveIcon = "none"' "$checkout/selfdrive/ui/ui_state.py" || fail "speed-limit icon default missing for ${label}"
  if [[ "$label" == sunnypilot* ]]; then
    grep -Fq 'from cereal import messaging, car, log, custom' "$checkout/selfdrive/ui/ui_state.py" || fail "custom import missing for ${label}"
    grep -Fq 'def _commaview_speed_limit_pre_active_icon(self) -> str:' "$checkout/selfdrive/ui/ui_state.py" || fail "speed-limit icon helper missing for ${label}"
    grep -Fq 'status.speedLimitPreActive = speed_limit_assist.state == custom.LongitudinalPlanSP.SpeedLimit.AssistState.preActive' "$checkout/selfdrive/ui/ui_state.py" || fail "speed-limit pre-active export missing for ${label}"
    grep -Fq 'status.speedLimitPreActiveIcon = self._commaview_speed_limit_pre_active_icon()' "$checkout/selfdrive/ui/ui_state.py" || fail "speed-limit icon export missing for ${label}"
  fi

  printf '%s\n' '{"healthy":false,"patchVerified":true,"statusScope":"patch-installation","controlServicePresent":true,"sceneServicePresent":true,"statusServicePresent":true,"schemaPresent":true,"runtimeFlavorFieldPresent":true,"statusModeEnumPresent":true,"statusModeFieldPresent":true,"speedLimitIconEnumPresent":true,"speedLimitPreActiveFieldPresent":true,"speedLimitPreActiveIconFieldPresent":true,"latActiveFieldPresent":true,"longActiveFieldPresent":true,"controlPublisherPresent":true,"scenePublisherPresent":true,"statusPublisherPresent":true,"runtimeFlavorConstantPresent":true,"runtimeFlavorPublisherPresent":true,"statusModeHelperPresent":true,"statusModePublisherPresent":true,"speedLimitDefaultsPresent":true,"speedLimitFlavorMarkersPresent":true,"latLongPublisherPresent":true,"controlEventPresent":true,"sceneEventPresent":true,"statusEventPresent":true}'

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

run_ref 'openpilot release-mici-staging' "$OPENPILOT_REPO" 'release-mici-staging' "$CACHE_ROOT/openpilot-release-mici-staging" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'openpilot nightly' "$OPENPILOT_REPO" 'nightly' "$CACHE_ROOT/openpilot-nightly" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'sunnypilot staging' "$SUNNYPILOT_REPO" 'staging' "$CACHE_ROOT/sunnypilot-staging" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'
run_ref 'sunnypilot dev' "$SUNNYPILOT_REPO" 'dev' "$CACHE_ROOT/sunnypilot-dev" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'

echo 'PASS: direct v2 UI export patch applies and verifies on real canary refs'
