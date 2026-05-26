import json
import re
import shlex
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULTS = REPO_ROOT / "comma4" / "runtime-debug.defaults.json"
START_SH = REPO_ROOT / "comma4" / "start.sh"
STOP_SH = REPO_ROOT / "comma4" / "stop.sh"
INSTALL_SH = REPO_ROOT / "comma4" / "install.sh"
UNINSTALL_SH = REPO_ROOT / "comma4" / "uninstall.sh"
APPLY_PATCH_SH = REPO_ROOT / "comma4" / "scripts" / "apply_onroad_ui_export_patch.sh"
REVERT_PATCH_SH = REPO_ROOT / "comma4" / "scripts" / "revert_onroad_ui_export_patch.sh"
VERIFY_PATCH_SH = REPO_ROOT / "comma4" / "scripts" / "verify_onroad_ui_export_patch.sh"
SUNNYPILOT_PATCH = REPO_ROOT / "comma4" / "patches" / "sunnypilot" / "0001-commaview-ui-export-v2.patch"
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
        "onroadProjection": {"mode": "pass"},
        "wideRoadCameraState": {"mode": "pass"},
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
    assert '"onroadProjection":{"mode":"pass"}' in text
    assert '"wideRoadCameraState":{"mode":"pass"}' in text

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
    assert "ensure_commaview_stopped" in text
    assert 'pkill -f "/data/commaview/commaviewd"' not in text
    assert 'INSTALL_SUCCESS=1' in text
    assert text.index('echo "Staging and validating bundle..."') < text.index('echo "Stopping existing CommaView processes..."')
    assert text.index('backup_managed_install_tree') < text.index('echo "Stopping existing CommaView processes..."')
    assert text.index('ensure_commaview_stopped') < text.index('clean_managed_install_tree')


def test_stop_script_kills_only_runtime_processes_without_pkill_self_match():
    text = STOP_SH.read_text()
    assert "commaview_pids()" in text
    assert '/proc/[0-9]*' in text
    assert '"/data/commaview/commaviewd bridge"' in text
    assert '"/data/commaview/commaviewd control"' in text
    assert 'pkill -f' not in text
    assert 'kill -9 $pids' in text
    assert 'ERROR: CommaView runtime processes still running' in text


def test_install_script_uses_robust_runtime_stop_before_mutating_live_tree():
    text = INSTALL_SH.read_text()
    assert "commaview_pids()" in text
    assert "stop_commaview_processes()" in text
    assert "ensure_commaview_stopped" in text
    assert 'kill -9 $pids' in text
    assert 'rm -f "$INSTALL_DIR/run/bridge.pid" "$INSTALL_DIR/run/control.pid"' in text
    stop_call = text.index('echo "Stopping existing CommaView processes..."')
    clean_call = text.index('\nclean_managed_install_tree', stop_call)
    assert stop_call < clean_call


def _shell_function_body(text: str, name: str) -> str:
    match = re.search(rf"\n{name}\(\) \{{(?P<body>.*?)\n\}}", text, re.S)
    assert match, f"missing shell function {name}"
    return match.group("body")


def _shell_function_definition(text: str, name: str) -> str:
    return f"{name}() {{{_shell_function_body(text, name)}\n}}"


def test_install_script_rolls_back_managed_src_tree():
    text = INSTALL_SH.read_text()
    backup_body = _shell_function_body(text, "backup_managed_install_tree")
    clean_body = _shell_function_body(text, "clean_managed_install_tree")
    restore_body = _shell_function_body(text, "restore_previous_install_tree")
    deploy_body = _shell_function_body(text, "deploy_required_scripts")

    assert "src" in backup_body
    assert '"$INSTALL_DIR/src"' in clean_body
    assert '"$INSTALL_DIR/src"' in restore_body
    assert 'mkdir -p "$INSTALL_DIR/src"' in deploy_body
    assert deploy_body.index('mkdir -p "$INSTALL_DIR/src"') < deploy_body.index('copy_required_file "src/commaview_export.openpilot.py"')


def test_install_script_failed_rollback_copy_preserves_backup_and_skips_restart():
    text = INSTALL_SH.read_text()
    restore_body = _shell_function_body(text, "restore_previous_install_tree")
    preserve_body = _shell_function_body(text, "preserve_install_rollback_backup")
    cleanup_body = _shell_function_body(text, "cleanup")

    assert 'INSTALL_ROLLBACK_BACKUP_ROOT="${COMMAVIEWD_INSTALL_ROLLBACK_BACKUP_ROOT:-$BACKUP_ROOT/install-rollback}"' in text
    assert 'BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"' in text
    assert 'preserved_dir="$(mktemp -d "$INSTALL_ROLLBACK_BACKUP_ROOT/$(date -u +%Y%m%d-%H%M%S).XXXXXX")" || return $?' in preserve_body
    assert 'cp -a "$backup_dir"/. "$preserved_dir"/ || return $?' in preserve_body
    assert "printf '%s\\n' \"$preserved_dir\" || return $?" in preserve_body
    assert 'PRESERVE_TMPDIR=1' in restore_body
    assert 'keeping temporary backup at $backup_dir' in restore_body
    assert 'if [ "$PRESERVE_TMPDIR" = "1" ]' in cleanup_body

    assert 'cp -a "$backup_dir"/. "$INSTALL_DIR"/ 2>/dev/null || true' not in restore_body
    assert not re.search(r'cp -a "\$backup_dir"/\. "\$INSTALL_DIR"/[^\n]*\|\|\s*true', restore_body)
    assert 'if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then' in restore_body
    assert 'return "$restore_ec"' in restore_body

    restore_copy = restore_body.index('if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then')
    restore_failure_return = restore_body.index('return "$restore_ec"')
    runtime_restart = restore_body.index('COMMAVIEWD_RESTART_REASON=install-rollback')
    assert restore_copy < restore_failure_return < runtime_restart


def test_preserve_install_rollback_backup_returns_nonzero_when_copy_fails_under_command_substitution(tmp_path):
    text = INSTALL_SH.read_text()
    function_definition = _shell_function_definition(text, "preserve_install_rollback_backup")
    bad_backup = tmp_path / "not-a-directory"
    bad_backup.touch()
    rollback_root = tmp_path / "rollback-root"
    script = f"""
set -euo pipefail
INSTALL_ROLLBACK_BACKUP_ROOT={shlex.quote(str(rollback_root))}
{function_definition}
preserved_dir=""
if preserved_dir="$(preserve_install_rollback_backup {shlex.quote(str(bad_backup))})"; then
  echo "unexpected success:$preserved_dir"
  exit 10
fi
if [ -n "$preserved_dir" ]; then
  echo "unexpected preserved path:$preserved_dir"
  exit 11
fi
"""

    result = subprocess.run(
        ["bash", "-c", script],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == ""


def test_install_script_preserves_patch_flavor_state_and_supports_explicit_current_reinstall():
    text = INSTALL_SH.read_text()
    assert "clean_managed_install_tree" in text
    assert '"$INSTALL_DIR/run"' in text
    assert '"$INSTALL_DIR/config/onroad-ui-export-patch.env"' not in text
    assert '"$INSTALL_DIR/config/hud-lite-patch.env"' in text
    assert "--current" in text
    assert "USE_CURRENT_RELEASE" in text
    assert 'RELEASE_TAG="$(resolve_latest_release_tag)"' in text
    assert "--force-offroad" in text
    assert 'write_param "OffroadMode" "1"' in text
    assert 'ensure_offroad_ready' in text
    assert 'apply_onroad_ui_export_patch.sh" --force-repair' in text


def test_install_script_prints_one_time_pair_code_after_starting_runtime():
    text = INSTALL_SH.read_text()
    assert 'print_pairing_code()' in text
    assert 'http://127.0.0.1:5002/pairing/create' in text
    assert 'X-CommaView-Token: $token' in text
    assert 'CommaView pair code:' in text
    assert text.index('bash "$INSTALL_DIR/start.sh"') < text.rindex('print_pairing_code')


def test_sunnypilot_patch_declares_every_managed_target_for_force_repair():
    text = SUNNYPILOT_PATCH.read_text()
    assert "+++ b/selfdrive/ui/commaview_export.py" in text
    assert "+++ b/selfdrive/ui/ui_state.py" in text
    assert "+++ b/selfdrive/ui/mici/onroad/augmented_road_view.py" in text
    assert text.index("+++ b/selfdrive/ui/mici/onroad/augmented_road_view.py") < text.index("self._update_commaview_camera_export()")
    assert "exporter.set_onroad_projection(" in text


def test_verify_patch_script_requires_onroad_projection_markers():
    text = VERIFY_PATCH_SH.read_text()
    assert "onroad_projection_present=false" in text
    assert '"onroadProjectionPresent"' in text
    assert "exporter.set_onroad_projection(" in text
    assert "self._latest_onroad_projection = {" in text
    assert '"modelTransform": _matrix3_list(model_transform)' in text
    assert "video_frame_matrix=self._cached_matrix" in text
    assert "$onroad_projection_present" in text


def test_apply_patch_script_refuses_implicit_destructive_repair_and_backs_up_force_repair():
    text = APPLY_PATCH_SH.read_text()
    assert "managed_targets()" in text
    assert "backup_managed_targets()" in text
    assert "dirty_managed_targets()" in text
    assert "reset_managed_targets()" in text
    assert "restore_managed_targets_from_backup()" in text
    assert "force_repair_managed_targets()" in text
    assert '--maintenance-mode' not in text
    assert 'COMMAVIEWD_MAINTENANCE_MODE' not in text
    assert '--force-repair' in text
    assert 'if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ]' in text
    assert 'onroad UI export transformer target files have local changes' in text
    assert 'refusing to modify dirty upstream files without --force-repair' in text
    assert 'backups written to $backup_root' in text
    assert 'transformer failed; restored managed targets' in text
    assert 'remote_flavor()' in text
    assert 'github.com:commaai/openpilot' in text
    assert 'github.com:sunnypilot/sunnypilot' in text
    assert 'github.com:sunnypilot/openpilot' in text
    assert 'selfdrive/ui/mici' in text
    assert 'git -C "$OP_ROOT" reset -q HEAD -- "$rel"' in text
    assert 'git -C "$OP_ROOT" checkout -- "$rel"' in text
    assert 'rm -f "$OP_ROOT/$rel"' in text
    assert '--force-offroad' in text
    assert 'write_param "OffroadMode" "1"' in text


def test_transformer_state_files_are_parsed_without_sourcing_shell():
    apply_text = APPLY_PATCH_SH.read_text()
    verify_text = VERIFY_PATCH_SH.read_text()
    assert '. "$STATE_ENV"' not in apply_text
    assert '. "$STATE_ENV"' not in verify_text
    assert 'source "$STATE_ENV"' not in apply_text
    assert 'source "$STATE_ENV"' not in verify_text
    assert 'state_value()' in apply_text
    assert 'state_value()' in verify_text
    assert 'unsupported upstream remote' in apply_text
    assert 'unsupported upstream remote' in verify_text
    assert 'json.dumps(payload' in verify_text


def test_uninstall_script_reverts_transformer_before_removing_install_tree():
    text = UNINSTALL_SH.read_text()
    assert "revert_onroad_ui_export_patch.sh" in text
    assert "Reverting direct v2 onroad UI export transformer" in text
    assert 'bash "$INSTALL_DIR/stop.sh"' in text
    assert 'rm -rf "$INSTALL_DIR"' in text
    assert '--force-offroad' in text
    assert 'revert_args+=(--force-offroad)' in text
    assert 'preserving $INSTALL_DIR for recovery' in text
    assert text.index('bash "$INSTALL_DIR/stop.sh"') < text.index("revert_onroad_ui_export_patch.sh")
    assert text.index("revert_onroad_ui_export_patch.sh") < text.index('rm -rf "$INSTALL_DIR"')


def test_revert_patch_script_resets_every_transformer_managed_target():
    text = REVERT_PATCH_SH.read_text()
    assert "managed_targets()" in text
    assert "backup_managed_targets()" in text
    assert "reset_managed_targets()" in text
    assert "restart_openpilot_ui_if_offroad" in text
    assert "COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups" in text
    assert "ensure_offroad_ready" in text
    assert "socket UI export transformer revert blocked while onroad" in text
    assert "--force-offroad" in text
    assert "selfdrive/ui/commaview_export.py" in text
    assert "selfdrive/ui/ui_state.py" in text
    assert "selfdrive/ui/mici/onroad/augmented_road_view.py" in text
    assert "selfdrive/ui/onroad/augmented_road_view.py" in text
    assert 'git -C "$OP_ROOT" reset -q HEAD -- "$rel"' in text
    assert 'git -C "$OP_ROOT" checkout -- "$rel"' in text
    assert 'rm -f "$OP_ROOT/$rel"' in text


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
    assert 'restarting openpilot UI to load CommaView onroad UI export transformer output' in text
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
    assert "roadState" in text
    assert "isOnroad" in text
    assert "onroad" in text
    assert "maintenance_mode_requested" not in text
    assert "--maintenance-mode" not in text
    assert "live_onroad_ui_export_status_json(false)" in text
    assert "run_onroad_ui_export_apply_status_json" in text
    assert 'json_field_true(request_body, "forceOffroad")' in text
