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
    assert "restart_openpilot_ui_if_pending" in text
    assert "onroad-ui-export-ui-restart-needed" in text
    assert "deferred onroad UI export restart still pending while onroad" in text
    assert "consuming deferred onroad UI export restart" in text
    assert "IsOnroad" in text


def test_install_script_stages_release_and_refreshes_pinned_companions_before_mutating_live_tree():
    text = INSTALL_SH.read_text()
    assert "refresh_required_files" in text
    assert 'COMPANION_DIR="$tmpdir/companions"' in text
    assert 'Fetching installer companion:' in text
    assert 'local src="$COMPANION_DIR/$src_rel"' in text
    assert "Staging and validating bundle" in text
    assert 'tar -xzf "$tmpdir/$ASSET_NAME" -C "$STAGED_BUNDLE" --strip-components=1' in text
    assert "backup_managed_install_tree" in text
    assert "restore_previous_install_tree" in text
    assert 'INSTALL_SUCCESS=1' in text
    assert text.index('echo "Staging and validating bundle..."') < text.index('echo "Stopping existing CommaView processes..."')
    assert text.index('backup_managed_install_tree') < text.index('echo "Stopping existing CommaView processes..."')


def test_install_script_cleans_transient_patch_state_and_supports_explicit_current_reinstall():
    text = INSTALL_SH.read_text()
    assert "clean_managed_install_tree" in text
    assert '"$INSTALL_DIR/run"' in text
    assert '"$INSTALL_DIR/config/onroad-ui-export-patch.env"' in text
    assert '"$INSTALL_DIR/config/hud-lite-patch.env"' in text
    assert "--current" in text
    assert "USE_CURRENT_RELEASE" in text
    assert 'RELEASE_TAG="$(resolve_latest_release_tag)"' in text
    assert "--force-offroad" in text
    assert 'write_param "OffroadMode" "1"' in text
    assert 'ensure_offroad_ready' in text


def test_install_script_prints_one_time_pair_code_after_starting_runtime():
    text = INSTALL_SH.read_text()
    assert 'print_pairing_code()' in text
    assert 'http://127.0.0.1:5002/pairing/create' in text
    assert 'X-CommaView-Token: $token' in text
    assert 'CommaView pair code:' in text
    assert text.index('bash "$INSTALL_DIR/start.sh"') < text.rindex('print_pairing_code')


def test_apply_patch_script_refuses_implicit_destructive_repair_and_backs_up_force_repair():
    text = APPLY_PATCH_SH.read_text()
    assert "patch_targets()" in text
    assert "backup_patch_targets()" in text
    assert "dirty_patch_targets()" in text
    assert "reset_patch_targets()" in text
    assert "force_repair_patch_targets()" in text
    assert '--maintenance-mode' not in text
    assert 'COMMAVIEWD_MAINTENANCE_MODE' not in text
    assert '--force-repair' in text
    assert 'if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ]' in text
    assert 'refusing to reset managed patch targets without --force-repair' in text
    assert 'upstream may have changed; review patch compatibility before repairing' in text
    assert 'onroad UI export patch target files have local changes' in text
    assert 'refusing to modify dirty upstream files without --force-repair' in text
    assert 'backups written to $backup_root' in text
    assert 'git -C "$OP_ROOT" reset -q HEAD -- "$rel"' in text
    assert 'git -C "$OP_ROOT" checkout -- "$rel"' in text
    assert 'rm -f "$OP_ROOT/$rel"' in text
    assert '--force-offroad' in text
    assert 'write_param "OffroadMode" "1"' in text


def test_apply_patch_script_restarts_openpilot_ui_after_patch_lifecycle_offroad_only():
    text = APPLY_PATCH_SH.read_text()
    assert "restart_openpilot_ui_if_offroad" in text
    assert "COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART" in text
    assert 'read_param IsOnroad' in text
    assert 'onroad-ui-export-ui-restart-needed' in text
    assert 'request_openpilot_ui_restart' in text
    assert 'deferring openpilot UI restart while onroad' in text
    assert 'pkill unavailable; deferring openpilot UI restart' in text
    assert 'pkill -INT -f "selfdrive.ui.ui"' in text
    assert 'restarting openpilot UI to load CommaView onroad UI export patch' in text
    assert text.index('restart_openpilot_ui_if_offroad') < text.index('if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json >/dev/null 2>&1; then')
    assert 'request_openpilot_ui_restart\n  restart_openpilot_ui_if_offroad\n  exit 0' in text
    assert 'request_openpilot_ui_restart\n  restart_openpilot_ui_if_offroad\n  exec "$VERIFY_SCRIPT" --json' in text


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
