import subprocess
import sys
import textwrap
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TRANSFORMER = REPO_ROOT / "comma4" / "scripts" / "transform_onroad_ui_export.py"

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"


def write_upstream_tree(tmp_path: Path, *, ui_state: str, augmented_road_view: str | None = None) -> Path:
    op_root = tmp_path / "openpilot"
    ui_dir = op_root / "selfdrive" / "ui"
    onroad_dir = ui_dir / "mici" / "onroad"
    ui_dir.mkdir(parents=True)
    onroad_dir.mkdir(parents=True)
    (ui_dir / "ui_state.py").write_text(textwrap.dedent(ui_state).lstrip())
    if augmented_road_view is not None:
        (onroad_dir / "augmented_road_view.py").write_text(textwrap.dedent(augmented_road_view).lstrip())
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


CAMERA_EXPORT_CALL = "self._update_commaview_camera_export()"
CAMERA_EXPORT_HELPER = "def _update_commaview_camera_export(self):"
PROJECTION_EXPORT_CALL = "exporter.set_onroad_projection("
MODEL_TRANSFORM_ASSIGN = "model_transform = video_transform @ calib_transform"
MODEL_TRANSFORM_SET = "self._model_renderer.set_transform(model_transform)"


def write_augmented_tree(tmp_path: Path, augmented_road_view: str = AUGMENTED_ROAD_VIEW) -> Path:
    return write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE, augmented_road_view=augmented_road_view)


def assert_augmented_transformed(augmented_path: Path) -> None:
    text = augmented_path.read_text()
    assert text.count(CAMERA_EXPORT_CALL) == 1
    assert text.count(CAMERA_EXPORT_HELPER) == 1
    assert text.count(MODEL_TRANSFORM_ASSIGN) == 1
    assert text.count(MODEL_TRANSFORM_SET) == 1
    assert text.count(PROJECTION_EXPORT_CALL) == 1
    assert text.index("super()._render(self._content_rect)") < text.index(CAMERA_EXPORT_CALL)
    assert text.index(MODEL_TRANSFORM_ASSIGN) < text.index(MODEL_TRANSFORM_SET) < text.index(PROJECTION_EXPORT_CALL)
    assert "video_frame_matrix=self._cached_matrix" in text
    assert "camera_offset=getattr(self._model_renderer, \"_camera_offset\", 0.0)" in text


def test_transformer_inserts_augmented_road_camera_and_projection_hooks(tmp_path):
    op_root = write_augmented_tree(tmp_path)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")


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
