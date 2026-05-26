import json
import os
import re
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TRANSFORMER = REPO_ROOT / "comma4" / "scripts" / "transform_onroad_ui_export.py"
APPLY_SCRIPT = REPO_ROOT / "comma4" / "scripts" / "apply_onroad_ui_export_patch.sh"
REVERT_SCRIPT = REPO_ROOT / "comma4" / "scripts" / "revert_onroad_ui_export_patch.sh"
OPENPILOT_TEMPLATE = REPO_ROOT / "comma4" / "src" / "commaview_export.openpilot.py"
SUNNYPILOT_TEMPLATE = REPO_ROOT / "comma4" / "src" / "commaview_export.sunnypilot.py"

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"
HELPER_PATH = Path("selfdrive/ui/commaview_export.py")


def write_upstream_tree(
    tmp_path: Path,
    *,
    ui_state: str,
    augmented_road_view: str | None = None,
    augmented_road_relpath: str = "selfdrive/ui/mici/onroad/augmented_road_view.py",
) -> Path:
    op_root = tmp_path / "openpilot"
    ui_dir = op_root / "selfdrive" / "ui"
    ui_dir.mkdir(parents=True)
    (ui_dir / "ui_state.py").write_text(textwrap.dedent(ui_state).lstrip())
    if augmented_road_view is not None:
        augmented_path = op_root / augmented_road_relpath
        augmented_path.parent.mkdir(parents=True, exist_ok=True)
        augmented_path.write_text(textwrap.dedent(augmented_road_view).lstrip())
    return op_root


def run_transformer(op_root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TRANSFORMER), "--op-root", str(op_root), "--flavor", "openpilot"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


OLD_UI_STATE = """
import time
from cereal import messaging, car, log
from openpilot.common.filter_simple import FirstOrderFilter
from openpilot.common.params import Params
from openpilot.common.swaglog import cloudlog
from openpilot.selfdrive.ui.lib.prime_state import PrimeState
from openpilot.system.ui.lib.application import gui_app
from openpilot.system.hardware import HARDWARE, PC

PARAM_UPDATE_TIME = 5.0

class UIState:
  def __init__(self):
    self._param_update_time = -PARAM_UPDATE_TIME

  def update(self) -> None:
    self.prime_state.start()
    self.sm.update(0)
    self._update_state()
    self._update_status()
    if time.monotonic() - self._param_update_time >= PARAM_UPDATE_TIME:
      self.update_params()
    device.update()

  def _update_state(self) -> None:
    pass
"""


NEW_UI_STATE = """
import time
import threading
from cereal import messaging, car, log
from openpilot.common.filter_simple import FirstOrderFilter
from openpilot.common.params import Params
from openpilot.common.realtime import drop_realtime
from openpilot.common.swaglog import cloudlog
from openpilot.selfdrive.ui.lib.prime_state import PrimeState
from openpilot.system.ui.lib.application import gui_app
from openpilot.system.hardware import HARDWARE, PC

PARAM_UPDATE_TIME = 1 / 5.0

class UIState:
  def __init__(self):
    self._params_thread = None

  def update(self) -> None:
    self.prime_state.start()
    if self._params_thread is None:
      self._params_thread = threading.Thread(target=self._params_refresh_worker, daemon=True)
      self._params_thread.start()

    self.sm.update(0)
    self._update_state()
    self._update_status()
    device.update()

  def _params_refresh_worker(self):
    drop_realtime()
    while True:
      self.update_params()
      time.sleep(PARAM_UPDATE_TIME)

  def _update_state(self) -> None:
    pass
"""


def assert_ui_state_transformed(ui_state_path: Path) -> None:
    text = ui_state_path.read_text()
    assert text.count(EXPORT_IMPORT) == 1
    assert text.count(EXPORT_INSTALL) == 1
    assert text.count(EXPORT_PUBLISH) == 1
    assert text.index("device.update()") < text.index(EXPORT_INSTALL) < text.index(EXPORT_PUBLISH)
    assert 'cloudlog.exception("commaview ui export publish failed")' in text


def test_transformer_handles_old_inline_params_ui_state_layout(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=OLD_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")


def test_transformer_installs_openpilot_export_helper_template(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=OLD_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    helper_text = (op_root / HELPER_PATH).read_text()
    assert 'COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"' in helper_text
    assert "class _CommaViewSocketExporter:" in helper_text


def test_transformer_handles_new_background_params_ui_state_layout(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")


def test_transformer_is_idempotent_for_ui_state(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)

    first = run_transformer(op_root)
    assert first.returncode == 0, first.stderr
    ui_state_path = op_root / "selfdrive" / "ui" / "ui_state.py"
    after_first = ui_state_path.read_text()

    second = run_transformer(op_root)
    assert second.returncode == 0, second.stderr

    assert ui_state_path.read_text() == after_first


def test_transformer_fails_when_ui_state_update_lacks_device_update(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE.replace("    device.update()\n", ""))

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "device.update" in result.stderr


def test_transformer_fails_when_ui_state_update_has_duplicate_device_update(tmp_path):
    duplicated = NEW_UI_STATE.replace("    device.update()\n", "    device.update()\n    device.update()\n")
    op_root = write_upstream_tree(tmp_path, ui_state=duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "device.update" in result.stderr


AUGMENTED_ROAD_VIEW = """
class AugmentedRoadView:
  def _render(self):
    # Render the base camera view
    super()._render(self._content_rect)

    # Draw all UI overlays
    self._model_renderer.render(self._content_rect)

  def _switch_stream_if_needed(self, sm):
    if sm['selfdriveState'].experimentalMode and WIDE_CAM in self.available_streams:
      target = WIDE_CAM
    else:
      target = ROAD_CAM

    if self.stream_type != target:
      self.switch_stream(target)

  def _update_calibration(self):
    sm = ui_state.sm
    if not (sm.updated["liveCalibration"] and sm.valid['liveCalibration']):
      return

  def _calc_frame_matrix(self, rect):
    is_wide_camera = self.stream_type == WIDE_CAM
    video_transform = make_video_transform()
    calib_transform = make_calib_transform()
    self._cached_matrix = make_cached_matrix()
    self._model_renderer.set_transform(video_transform @ calib_transform)

    return self._cached_matrix
"""


FLAT_AUGMENTED_ROAD_VIEW = """
class AugmentedRoadView:
  def _render(self, rect):
    self._content_rect = rect
    # Render the base camera view
    super()._render(rect)

    # Draw all UI overlays
    self.model_renderer.render(self._content_rect)

  def _switch_stream_if_needed(self, sm):
    if sm['selfdriveState'].experimentalMode and WIDE_CAM in self.available_streams:
      target = WIDE_CAM
    else:
      target = ROAD_CAM

    if self.stream_type != target:
      self.switch_stream(target)

  def _update_calibration(self):
    sm = ui_state.sm
    if not (sm.updated["liveCalibration"] and sm.valid['liveCalibration']):
      return

  def _calc_frame_matrix(self, rect):
    is_wide_camera = self.stream_type == WIDE_CAM
    video_transform = make_video_transform()
    calib_transform = make_calib_transform()
    self._cached_matrix = make_cached_matrix()
    self.model_renderer.set_transform(video_transform @ calib_transform)

    return self._cached_matrix
"""


CAMERA_EXPORT_CALL = "self._update_commaview_camera_export()"
CAMERA_EXPORT_HELPER = "def _update_commaview_camera_export(self):"
PROJECTION_EXPORT_CALL = "exporter.set_onroad_projection("
MODEL_TRANSFORM_ASSIGN = "model_transform = video_transform @ calib_transform"
MODEL_TRANSFORM_SET = "self._model_renderer.set_transform(model_transform)"
FLAT_MODEL_TRANSFORM_SET = "self.model_renderer.set_transform(model_transform)"


def write_augmented_tree(
    tmp_path: Path,
    augmented_road_view: str = AUGMENTED_ROAD_VIEW,
    augmented_road_relpath: str = "selfdrive/ui/mici/onroad/augmented_road_view.py",
) -> Path:
    return write_upstream_tree(
        tmp_path,
        ui_state=NEW_UI_STATE,
        augmented_road_view=augmented_road_view,
        augmented_road_relpath=augmented_road_relpath,
    )


def assert_augmented_transformed(augmented_path: Path) -> None:
    text = augmented_path.read_text()
    assert text.count(CAMERA_EXPORT_CALL) == 1
    assert text.count(CAMERA_EXPORT_HELPER) == 1
    assert text.count(MODEL_TRANSFORM_ASSIGN) == 1
    assert text.count(MODEL_TRANSFORM_SET) + text.count(FLAT_MODEL_TRANSFORM_SET) == 1
    assert text.count(PROJECTION_EXPORT_CALL) == 1
    render_anchor = "super()._render(self._content_rect)" if "super()._render(self._content_rect)" in text else "super()._render(rect)"
    transform_set = MODEL_TRANSFORM_SET if MODEL_TRANSFORM_SET in text else FLAT_MODEL_TRANSFORM_SET
    assert text.index(render_anchor) < text.index(CAMERA_EXPORT_CALL)
    assert text.index(MODEL_TRANSFORM_ASSIGN) < text.index(transform_set) < text.index(PROJECTION_EXPORT_CALL)
    assert "video_frame_matrix=self._cached_matrix" in text
    assert (
        "camera_offset=getattr(self._model_renderer, \"_camera_offset\", 0.0)" in text
        or "camera_offset=getattr(self.model_renderer, \"_camera_offset\", 0.0)" in text
    )


def test_transformer_inserts_augmented_road_camera_and_projection_hooks(tmp_path):
    op_root = write_augmented_tree(tmp_path)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")


def test_transformer_handles_flat_onroad_augmented_road_view_path(tmp_path):
    op_root = write_augmented_tree(
        tmp_path,
        FLAT_AUGMENTED_ROAD_VIEW,
        augmented_road_relpath="selfdrive/ui/onroad/augmented_road_view.py",
    )

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")


def test_transformer_is_idempotent_for_augmented_road_view(tmp_path):
    op_root = write_augmented_tree(tmp_path)
    augmented_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"

    first = run_transformer(op_root)
    assert first.returncode == 0, first.stderr
    after_first = augmented_path.read_text()

    second = run_transformer(op_root)
    assert second.returncode == 0, second.stderr

    assert augmented_path.read_text() == after_first


def test_transformer_fails_when_augmented_render_anchor_is_missing(tmp_path):
    op_root = write_augmented_tree(tmp_path, AUGMENTED_ROAD_VIEW.replace("    super()._render(self._content_rect)\n", ""))

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "super()._render" in result.stderr


def test_transformer_fails_when_augmented_render_anchor_is_duplicated(tmp_path):
    duplicated = AUGMENTED_ROAD_VIEW.replace(
        "    super()._render(self._content_rect)\n",
        "    super()._render(self._content_rect)\n    super()._render(self._content_rect)\n",
    )
    op_root = write_augmented_tree(tmp_path, duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "super()._render" in result.stderr


def test_transformer_fails_when_augmented_transform_anchor_is_missing(tmp_path):
    op_root = write_augmented_tree(
        tmp_path,
        AUGMENTED_ROAD_VIEW.replace("    self._model_renderer.set_transform(video_transform @ calib_transform)\n", ""),
    )

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "set_transform(video_transform @ calib_transform)" in result.stderr


def test_transformer_fails_when_augmented_transform_anchor_is_duplicated(tmp_path):
    duplicated = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
    )
    op_root = write_augmented_tree(tmp_path, duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "set_transform(video_transform @ calib_transform)" in result.stderr


def init_git_repo(op_root: Path) -> None:
    subprocess.run(["git", "init"], cwd=op_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run(["git", "config", "user.email", "commaview-test@example.invalid"], cwd=op_root, check=True)
    subprocess.run(["git", "config", "user.name", "CommaView Test"], cwd=op_root, check=True)
    subprocess.run(["git", "add", "."], cwd=op_root, check=True)
    subprocess.run(["git", "commit", "-m", "baseline"], cwd=op_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def prepare_lifecycle_install_dir(tmp_path: Path, op_root: Path) -> Path:
    install_dir = tmp_path / "commaview"
    (install_dir / "scripts").mkdir(parents=True)
    (install_dir / "src").mkdir(parents=True)
    (install_dir / "config").mkdir(parents=True)
    shutil.copy2(TRANSFORMER, install_dir / "scripts" / "transform_onroad_ui_export.py")
    shutil.copy2(APPLY_SCRIPT, install_dir / "scripts" / "apply_onroad_ui_export_patch.sh")
    shutil.copy2(REVERT_SCRIPT, install_dir / "scripts" / "revert_onroad_ui_export_patch.sh")
    shutil.copy2(OPENPILOT_TEMPLATE, install_dir / "src" / "commaview_export.openpilot.py")
    shutil.copy2(SUNNYPILOT_TEMPLATE, install_dir / "src" / "commaview_export.sunnypilot.py")
    (install_dir / "config" / "onroad-ui-export-patch.env").write_text(
        f"ONROAD_UI_EXPORT_FLAVOR=openpilot\nONROAD_UI_EXPORT_OP_ROOT={op_root}\n"
    )
    return install_dir


def lifecycle_env(install_dir: Path, op_root: Path, **extra: str) -> dict[str, str]:
    return {
        "COMMAVIEWD_INSTALL_DIR": str(install_dir),
        "COMMAVIEWD_OP_ROOT": str(op_root),
        "COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART": "1",
        **extra,
    }


def run_lifecycle_script(script: Path, install_dir: Path, op_root: Path, *args: str, **extra_env: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script), *args],
        env={**lifecycle_env(install_dir, op_root, **extra_env)},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def write_full_augmented_tree(tmp_path: Path) -> Path:
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)
    mici_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"
    flat_path = op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py"
    mici_path.parent.mkdir(parents=True, exist_ok=True)
    flat_path.parent.mkdir(parents=True, exist_ok=True)
    mici_path.write_text(textwrap.dedent(AUGMENTED_ROAD_VIEW).lstrip())
    flat_path.write_text(textwrap.dedent(FLAT_AUGMENTED_ROAD_VIEW).lstrip())
    return op_root


def test_apply_script_force_repair_resets_dirty_targets_then_reapplies_transformer(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    ui_state.write_text(ui_state.read_text() + "\n# user dirty change\n")

    refused = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert refused.returncode == 44
    assert "refusing to modify dirty upstream files without --force-repair" in refused.stderr

    repaired = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert repaired.returncode == 0, repaired.stderr
    assert_ui_state_transformed(ui_state)
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")
    assert (op_root / HELPER_PATH).exists()


def test_revert_script_removes_helper_and_restores_tracked_transformer_targets(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    originals = {
        rel: (op_root / rel).read_text()
        for rel in (
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    assert (op_root / HELPER_PATH).exists()

    reverted = run_lifecycle_script(REVERT_SCRIPT, install_dir, op_root)

    assert reverted.returncode == 0, reverted.stderr
    assert not (op_root / HELPER_PATH).exists()
    for rel, text in originals.items():
        assert (op_root / rel).read_text() == text
    status = subprocess.check_output(
        ["git", "status", "--short", "--", "selfdrive/ui/commaview_export.py", "selfdrive/ui/ui_state.py", "selfdrive/ui/mici/onroad/augmented_road_view.py", "selfdrive/ui/onroad/augmented_road_view.py"],
        cwd=op_root,
        text=True,
    )
    assert status == ""


def test_apply_script_rolls_back_partial_transformer_failure(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    augmented_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"
    original_ui_state = ui_state.read_text()
    original_augmented = augmented_path.read_text()

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode != 0
    assert "transformer failed; restored managed targets" in result.stderr
    assert ui_state.read_text() == original_ui_state
    assert augmented_path.read_text() == original_augmented
    assert not (op_root / HELPER_PATH).exists()
    status = subprocess.check_output(
        ["git", "status", "--short", "--", "selfdrive/ui/commaview_export.py", "selfdrive/ui/ui_state.py", "selfdrive/ui/mici/onroad/augmented_road_view.py"],
        cwd=op_root,
        text=True,
    )
    assert status == ""


def test_apply_script_force_repair_and_transform_failure_use_distinct_backups(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    ui_state.write_text(ui_state.read_text() + "\n# dirty local user change\n")

    fakebin = tmp_path / "fakebin"
    fakebin.mkdir()
    fake_date = fakebin / "date"
    fake_date.write_text("#!/bin/sh\necho 20260526-132500\n")
    fake_date.chmod(0o755)

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        "--force-repair",
        PATH=f"{fakebin}:{os.environ['PATH']}",
    )

    assert result.returncode != 0
    backup_paths = re.findall(r"backups written to (\S+)", result.stderr)
    restore_paths = re.findall(r"restored managed targets from (\S+)", result.stderr)
    assert backup_paths
    assert restore_paths
    all_paths = backup_paths + restore_paths
    assert len(set(all_paths)) >= 2, result.stderr


def test_revert_script_writes_backup_outside_install_tree(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    backup_root = tmp_path / "surviving-backups"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr

    reverted = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        COMMAVIEWD_BACKUP_ROOT=str(backup_root),
    )

    assert reverted.returncode == 0, reverted.stderr
    shutil.rmtree(install_dir)
    backups = list((backup_root / "onroad-ui-export-revert").glob("*"))
    assert backups
    assert any((backup / "selfdrive" / "ui" / "ui_state.py").exists() for backup in backups)


def test_apply_script_accepts_sunnypilot_openpilot_remote(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/sunnypilot/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    assert "ONROAD_UI_EXPORT_FLAVOR=sunnypilot" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()


def test_apply_script_rejects_unsupported_upstream_fork_even_with_state_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/example/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stderr
    assert "commaai/openpilot and sunnypilot" in result.stderr


def test_verify_script_rejects_unsupported_upstream_fork_even_with_state_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "git@github.com:example/sunnypilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(REPO_ROOT / "comma4" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stdout
    assert "commaai/openpilot and sunnypilot" in result.stdout


def test_verify_script_fails_when_existing_flat_augmented_road_target_is_stale(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    flat_augmented_path = op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")
    assert_augmented_transformed(flat_augmented_path)

    subprocess.run(
        ["git", "checkout", "HEAD", "--", "selfdrive/ui/onroad/augmented_road_view.py"],
        cwd=op_root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    result = run_lifecycle_script(REPO_ROOT / "comma4" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert result.returncode == 1
    status = json.loads(result.stdout)
    assert status["patchVerified"] is False
    assert status["repairNeeded"] is True
