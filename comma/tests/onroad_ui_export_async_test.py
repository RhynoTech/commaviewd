import importlib.util
import json
import socket as socket_lib
import sys
import threading
import time
import types
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = (
  (REPO_ROOT / "comma" / "src" / "commaview_export.openpilot.py", "OPENPILOT"),
  (REPO_ROOT / "comma" / "src" / "commaview_export.sunnypilot.py", "SUNNYPILOT"),
)

WIRE_INDEXES = {
  "COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX": 0,
  "COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX": 1,
  "COMMAVIEW_CAR_STATE_SERVICE_INDEX": 2,
  "COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX": 3,
  "COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX": 4,
  "COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX": 5,
  "COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX": 6,
  "COMMAVIEW_MODEL_V2_SERVICE_INDEX": 7,
  "COMMAVIEW_RADAR_STATE_SERVICE_INDEX": 8,
  "COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX": 9,
  "COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX": 10,
  "COMMAVIEW_CAR_CONTROL_SERVICE_INDEX": 11,
  "COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX": 12,
  "COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX": 13,
  "COMMAVIEW_CAR_PARAMS_SERVICE_INDEX": 14,
  "COMMAVIEW_DEVICE_STATE_SERVICE_INDEX": 15,
  "COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX": 16,
  "COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX": 17,
  "COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX": 18,
  "COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX": 19,
}


def load_exporter(path: Path):
  sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
  car_module = types.ModuleType("opendbc.car")
  car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
  sys.modules["opendbc.car"] = car_module
  spec = importlib.util.spec_from_file_location(f"commaview_export_{path.stem}_{time.time_ns()}", path)
  assert spec is not None and spec.loader is not None
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


class FakeSubMaster:
  def __init__(self, values):
    self.values = values
    self.recv_frame = {key: 10 for key in values}
    self.logMonoTime = {key: 1000 + offset for offset, key in enumerate(values)}
    self.valid = {key: True for key in values}
    self.alive = {key: True for key in values}
    self.updated = {key: True for key in values}

  def __getitem__(self, key):
    return self.values[key]


class FakeUiState:
  def __init__(self, values):
    self.started_frame = 1
    self.sm = FakeSubMaster(values)
    self.CP = types.SimpleNamespace(maxLateralAccel=3.0)


def alias_values(current: bool):
  calibration = "extrinsicsCalibration" if current else "liveCalibration"
  road_camera = "narrowRoadCameraState" if current else "roadCameraState"
  vehicle_parameters = "vehicleParameters" if current else "liveParameters"
  return {
    calibration: types.SimpleNamespace(
      rpyCalib=[0.1, 0.2, 0.3],
      height=[1.22],
      calStatus=2,
      calPerc=87,
      wideFromDeviceEuler=[0.4, 0.5, 0.6],
    ),
    road_camera: types.SimpleNamespace(sensor="ar0231", frameId=4242, timestampEof=9999),
    vehicle_parameters: types.SimpleNamespace(roll=0.125),
  }


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_wire_service_indexes_remain_stable(template, flavor):
  module = load_exporter(template)
  assert {name: getattr(module, name) for name in WIRE_INDEXES} == WIRE_INDEXES


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_json_encoder_falls_back_to_stdlib_and_preserves_compact_unicode(template, flavor, monkeypatch):
  module = load_exporter(template)
  monkeypatch.setattr(module, "_orjson", None)

  assert module._encode_json({"label": "café", "ok": True}) == b'{"label":"caf\xc3\xa9","ok":true}'


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_json_encoder_uses_orjson_when_available(template, flavor, monkeypatch):
  module = load_exporter(template)
  calls = []

  class FakeOrjson:
    @staticmethod
    def dumps(payload):
      calls.append(payload)
      return b'{"fast":true}'

  monkeypatch.setattr(module, "_orjson", FakeOrjson())

  assert module._encode_json({"fast": True}) == b'{"fast":true}'
  assert calls == [{"fast": True}]


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_release_and_current_aliases_emit_equivalent_wire_semantics(template, flavor):
  module = load_exporter(template)
  legacy_exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  current_exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  legacy = FakeUiState(alias_values(current=False))
  current = FakeUiState(alias_values(current=True))

  assert legacy_exporter._live_calibration_payload(legacy) == current_exporter._live_calibration_payload(current)
  assert legacy_exporter._road_camera_state_payload(legacy) == current_exporter._road_camera_state_payload(current)
  assert legacy_exporter._live_parameters_payload(legacy) == current_exporter._live_parameters_payload(current)
  assert module.COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX == 9
  assert module.COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX == 12
  assert module.COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX == 16


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_current_vehicle_parameters_support_angle_control_torque(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "controlsState": types.SimpleNamespace(
      lateralControlState=types.SimpleNamespace(which=lambda: "angleState"),
      curvature=0.02,
      desiredCurvature=0.03,
    ),
    "carState": types.SimpleNamespace(vEgo=10.0),
    "carControl": types.SimpleNamespace(latActive=True),
  })

  torque = module._torque_bar_value(FakeUiState(values), exporter._service_resolver)

  assert torque == pytest.approx(0.795625, abs=1e-4)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_current_projection_uses_narrow_camera_and_extrinsics_metadata(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5252, timestampEof=8888),
    "modelV2": types.SimpleNamespace(frameId=6262, timestampEof=7777),
  })
  ui_state = FakeUiState(values)

  exporter.set_onroad_projection(
    ui_state,
    "road",
    types.SimpleNamespace(x=1, y=2, width=3, height=4),
    [1.0] * 9,
    [2.0] * 9,
  )

  payload = exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)
  assert payload["roadFrameId"] == 4242
  assert payload["roadTimestampEof"] == 9999
  assert payload["liveCalibrationLogMonoTime"] == ui_state.sm.logMonoTime["extrinsicsCalibration"]
  assert payload["logMonoTime"] == max(ui_state.sm.logMonoTime.values())


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_missing_semantic_alias_fails_explicitly(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)

  with pytest.raises(KeyError, match="calibration.*extrinsicsCalibration.*liveCalibration"):
    exporter._service_resolver.resolve(FakeUiState({}), "calibration")


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_publish_path_performs_no_json_or_socket_io_on_calling_thread(template, flavor, monkeypatch):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  ui_thread = threading.get_ident()
  violations = []
  original_dumps = json.dumps

  def guarded_dumps(*args, **kwargs):
    if threading.get_ident() == ui_thread:
      violations.append("json.dumps")
    return original_dumps(*args, **kwargs)

  class GuardedSocket:
    def __init__(self, *args, **kwargs):
      if threading.get_ident() == ui_thread:
        violations.append("socket.socket")
      raise OSError("receiver absent")

  monkeypatch.setattr(module.json, "dumps", guarded_dumps)
  monkeypatch.setattr(module.socket, "socket", GuardedSocket)
  values = alias_values(current=True)
  for service_index, _, _ in exporter._service_specs:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}

  ui_state = FakeUiState(values)
  exporter.set_onroad_projection(ui_state, "road", types.SimpleNamespace(), [1.0] * 9, [1.0] * 9)
  exporter.publish(ui_state)
  assert exporter.wait_for_idle(1.0)
  exporter.shutdown()

  assert violations == []


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_latest_value_mailbox_is_bounded_and_drops_stale_generations(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)

  started = time.perf_counter()
  for generation in range(10_000):
    exporter._offer_payload(7, {"generation": generation})
    exporter._offer_payload(8, {"generation": generation})
  elapsed = time.perf_counter() - started

  assert len(exporter._pending) == 2
  assert exporter._pending_payload(7) == {"generation": 9_999}
  assert exporter.stats()["maxPending"] == 2
  assert exporter.stats()["overwritten"] == 19_998
  assert elapsed < 0.25


@pytest.mark.parametrize("receiver_mode", ("never-reads", "slow-reader", "disconnect-mid-frame"))
@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_socket_backpressure_cannot_block_latest_value_offer(template, flavor, receiver_mode):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  sender, receiver = socket_lib.socketpair()
  sender.setsockopt(socket_lib.SOL_SOCKET, socket_lib.SO_SNDBUF, 4096)
  sender.settimeout(0.05)
  exporter._sock = sender
  stop_reader = threading.Event()
  reader = None

  if receiver_mode == "disconnect-mid-frame":
    receiver.close()
  elif receiver_mode == "slow-reader":
    def read_slowly():
      while not stop_reader.is_set():
        try:
          if not receiver.recv(256):
            return
        except OSError:
          return
        time.sleep(0.005)
    reader = threading.Thread(target=read_slowly, daemon=True)
    reader.start()

  oversized = {"generation": 0, "blob": "x" * 1_000_000}
  exporter._offer_payload(7, oversized)
  time.sleep(0.01)
  started = time.perf_counter()
  for generation in range(1, 1_001):
    exporter._offer_payload(7, {"generation": generation, "blob": oversized["blob"]})
  offer_elapsed = time.perf_counter() - started

  assert offer_elapsed < 0.1
  assert exporter.stats()["pending"] <= 1
  assert exporter.stats()["maxPending"] <= 1
  assert exporter.wait_for_idle(2.0)
  exporter.shutdown()
  stop_reader.set()
  try:
    receiver.close()
  except OSError:
    pass
  if reader is not None:
    reader.join(0.2)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_worker_survives_serialization_error_and_sends_next_service(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  packets = []

  class SinkSocket:
    def sendall(self, packet):
      packets.append(packet)

    def close(self):
      pass

  exporter._sock = SinkSocket()
  exporter._offer_payload(1, {"bad": {object()}})
  exporter._offer_payload(2, {"good": True})

  assert exporter.wait_for_idle(1.0)
  stats = exporter.stats()
  exporter.shutdown()

  assert stats["workerExceptions"] == 1
  assert stats["sent"] == 1
  assert len(packets) == 1
  assert packets[0][5] == 2


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_repeated_disconnects_and_receiver_restart_do_not_kill_worker(template, flavor, monkeypatch):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  created = []

  class RestartingSocket:
    def __init__(self):
      self.number = len(created)
      self.sent = []
      created.append(self)

    def settimeout(self, _timeout):
      pass

    def connect(self, _path):
      if self.number == 0:
        raise OSError("receiver absent")

    def sendall(self, packet):
      if self.number == 1:
        raise OSError("receiver restarted mid-frame")
      self.sent.append(packet)

    def close(self):
      pass

  monkeypatch.setattr(module.socket, "socket", lambda *_args: RestartingSocket())
  monkeypatch.setattr(module, "COMMAVIEW_CONNECT_RETRY_SEC", 0.0)

  for generation in range(3):
    exporter._offer_payload(7, {"generation": generation})
    assert exporter.wait_for_idle(1.0)
  exporter.shutdown()

  assert len(created) == 3
  assert len(created[2].sent) == 1


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_projection_uses_one_latest_value_path_without_publish_duplication(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5, timestampEof=6),
    "modelV2": types.SimpleNamespace(frameId=7, timestampEof=8),
  })
  ui_state = FakeUiState(values)

  for road_frame in (100, 101):
    values["narrowRoadCameraState"].frameId = road_frame
    exporter.set_onroad_projection(ui_state, "road", types.SimpleNamespace(), [1.0] * 9, [1.0] * 9)
    time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
  before_publish = exporter.stats().copy()
  for service_index in exporter._payload_builders:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}
  exporter.publish(ui_state)

  assert exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)["roadFrameId"] == 101
  assert before_publish["offers"] == 2
  assert before_publish["overwritten"] == 1
  assert exporter.stats()["overwritten"] == 1


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_unchanged_publish_path_stays_below_host_timing_gate(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  ui_state = FakeUiState(alias_values(current=True))
  for service_index in exporter._payload_builders:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}

  for _ in range(500):
    exporter.publish(ui_state)

  stats = exporter.stats()
  assert stats["snapshotP99Ms"] < 1.0
  assert stats["snapshotMaxMs"] < 5.0
  assert stats["maxPending"] <= len(exporter._service_specs)
