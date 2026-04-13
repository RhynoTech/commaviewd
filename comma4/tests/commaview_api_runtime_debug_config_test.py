import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULTS = REPO_ROOT / "comma4" / "runtime-debug.defaults.json"
START_SH = REPO_ROOT / "comma4" / "start.sh"
INSTALL_SH = REPO_ROOT / "comma4" / "install.sh"
APPLY_PATCH_SH = REPO_ROOT / "comma4" / "scripts" / "apply_onroad_ui_export_patch.sh"
CONTROL_CPP = REPO_ROOT / "commaviewd" / "src" / "control_mode.cpp"


def test_runtime_debug_defaults_match_policy_contract():
    data = json.loads(DEFAULTS.read_text())
    assert data["configVersion"] == 1
    assert data["instrumentationLevel"] == "standard"
    services = data["services"]
    assert services == {
        "uiStateOnroad": {"mode": "pass"},
        "selfdriveState": {"mode": "pass"},
        "carState": {"mode": "pass"},
        "controlsState": {"mode": "pass"},
        "onroadEvents": {"mode": "pass"},
        "driverMonitoringState": {"mode": "pass"},
        "driverStateV2": {"mode": "pass"},
        "modelV2": {"mode": "pass"},
        "radarState": {"mode": "pass"},
        "liveCalibration": {"mode": "pass"},
        "carOutput": {"mode": "pass"},
        "carControl": {"mode": "pass"},
        "liveParameters": {"mode": "pass"},
        "longitudinalPlan": {"mode": "pass"},
        "carParams": {"mode": "pass"},
        "deviceState": {"mode": "pass"},
        "roadCameraState": {"mode": "pass"},
        "pandaStatesSummary": {"mode": "pass"},
    }


def test_start_script_seeds_direct_v2_runtime_debug_defaults_and_exports_paths():
    text = START_SH.read_text()
    assert "runtime-debug.defaults.json" in text
    assert "RUNTIME_DEBUG_CONFIG" in text
    assert "RUNTIME_DEBUG_EFFECTIVE" in text
    assert "RUNTIME_DEBUG_STATS" in text
    assert "last-restart-reason.txt" in text
    assert "COMMAVIEWD_RUNTIME_DEBUG_CONFIG" in text
    assert "COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE" in text
    assert "COMMAVIEWD_RUNTIME_STATS" in text
    assert "invalid runtime debug config JSON" in text
    assert '"uiStateOnroad":{"mode":"pass"}' in text
    assert '"selfdriveState":{"mode":"pass"}' in text
    assert '"carState":{"mode":"pass"}' in text
    assert '"controlsState":{"mode":"pass"}' in text
    assert '"onroadEvents":{"mode":"pass"}' in text
    assert '"driverMonitoringState":{"mode":"pass"}' in text
    assert '"driverStateV2":{"mode":"pass"}' in text
    assert '"modelV2":{"mode":"pass"}' in text
    assert '"radarState":{"mode":"pass"}' in text
    assert '"liveCalibration":{"mode":"pass"}' in text
    assert '"carOutput":{"mode":"pass"}' in text
    assert '"carControl":{"mode":"pass"}' in text
    assert '"liveParameters":{"mode":"pass"}' in text
    assert '"longitudinalPlan":{"mode":"pass"}' in text
    assert '"carParams":{"mode":"pass"}' in text
    assert '"deviceState":{"mode":"pass"}' in text
    assert '"roadCameraState":{"mode":"pass"}' in text
    assert '"pandaStatesSummary":{"mode":"pass"}' in text

    for forbidden in (
        '"commaViewHudLite"',
        '"commaViewControl"',
        '"commaViewScene"',
        '"commaViewStatus"',
    ):
        assert forbidden not in text


def test_start_script_refreshes_and_self_heals_onroad_ui_export_status_offroad_only():
    text = START_SH.read_text()
    assert "verify_onroad_ui_export_patch.sh" in text
    assert "apply_onroad_ui_export_patch.sh" in text
    assert "refresh_onroad_ui_export_status" in text
    assert "onroad UI export patch verified at startup" in text
    assert "onroad UI export patch repaired at startup" in text
    assert "skipping startup repair" in text
    assert "IsOnroad" in text


def test_install_script_cleans_managed_artifacts_before_extracting_bundle():
    text = INSTALL_SH.read_text()
    assert "clean_managed_install_tree" in text
    assert 'rm -rf \\\n    "$INSTALL_DIR/lib"' in text
    assert '"$INSTALL_DIR/scripts"' in text
    assert '"$INSTALL_DIR/patches"' in text
    assert '"$INSTALL_DIR/config"' in text
    assert text.index('echo "Downloading release assets..."') < text.index('echo "Stopping existing CommaView processes..."')
    assert "--force-offroad" in text
    assert 'write_param "OffroadMode" "1"' in text
    assert 'ensure_offroad_ready' in text


def test_apply_patch_script_resets_managed_patch_targets_before_retry():
    text = APPLY_PATCH_SH.read_text()
    assert "patch_targets()" in text
    assert "reset_patch_targets()" in text
    assert '--maintenance-mode' not in text
    assert 'COMMAVIEWD_MAINTENANCE_MODE' not in text
    assert 'git -C "$OP_ROOT" reset -q HEAD -- "$rel"' in text
    assert 'git -C "$OP_ROOT" checkout -- "$rel"' in text
    assert 'rm -f "$OP_ROOT/$rel"' in text
    assert 'resetting managed onroad UI export patch targets before apply' in text
    assert '--force-offroad' in text
    assert 'write_param "OffroadMode" "1"' in text


def test_control_mode_routes_present_for_runtime_debug_config():
    text = CONTROL_CPP.read_text()
    assert "/commaview/runtime-debug/config" in text
    assert "/commaview/runtime-debug/defaults" in text
    assert "/commaview/runtime-debug/apply" in text
    assert "runtime_status_json" in text
    assert "runtime_debug_state_json" in text
    assert "runtime_debug_apply_response" in text
    assert "runtimeVersion" in text
    assert "maintenance_mode_requested" not in text
    assert "--maintenance-mode" not in text
    assert "live_onroad_ui_export_status_json(false)" in text
    assert "run_onroad_ui_export_apply_status_json" in text
    assert 'json_field_true(request_body, "forceOffroad")' in text
