import subprocess
import sys
import textwrap
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TRANSFORMER = REPO_ROOT / "comma4" / "scripts" / "transform_onroad_ui_export.py"

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"


def write_upstream_tree(tmp_path: Path, *, ui_state: str) -> Path:
    op_root = tmp_path / "openpilot"
    ui_dir = op_root / "selfdrive" / "ui"
    ui_dir.mkdir(parents=True)
    (ui_dir / "ui_state.py").write_text(textwrap.dedent(ui_state).lstrip())
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
