import json
import math
import os
import socket
import struct
import sys
import threading
import time
from collections import deque

from opendbc.car import ACCELERATION_DUE_TO_GRAVITY

COMMAVIEW_VENDOR_DIR = os.environ.get("COMMAVIEWD_PYTHON_VENDOR_DIR", "/data/commaview/vendor")
if os.path.isdir(COMMAVIEW_VENDOR_DIR) and COMMAVIEW_VENDOR_DIR not in sys.path:
  sys.path.insert(0, COMMAVIEW_VENDOR_DIR)

try:
  import orjson as _orjson
except (ImportError, OSError):
  _orjson = None

COMMAVIEW_RUNTIME_FLAVOR = "SUNNYPILOT"
COMMAVIEW_FRAME_VERSION = 1
COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX = 0
COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX = 1
COMMAVIEW_CAR_STATE_SERVICE_INDEX = 2
COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX = 3
COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX = 4
COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX = 5
COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX = 6
COMMAVIEW_MODEL_V2_SERVICE_INDEX = 7
COMMAVIEW_RADAR_STATE_SERVICE_INDEX = 8
COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX = 9
COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX = 10
COMMAVIEW_CAR_CONTROL_SERVICE_INDEX = 11
COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX = 12
COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX = 13
COMMAVIEW_CAR_PARAMS_SERVICE_INDEX = 14
COMMAVIEW_DEVICE_STATE_SERVICE_INDEX = 15
COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX = 16
COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX = 17
COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX = 18
COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX = 19
COMMAVIEW_RECIPE_EVENT_SERVICE_INDEX = 20
COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"
COMMAVIEW_CONNECT_RETRY_SEC = 1.0
COMMAVIEW_SOCKET_TIMEOUT_SEC = 0.01
COMMAVIEW_DEFAULT_MAX_LAT_ACCEL = 3.0
COMMAVIEW_MS_TO_KPH = 3.6
COMMAVIEW_MS_TO_MPH = 2.23694
COMMAVIEW_KM_TO_MILE = 0.621371
COMMAVIEW_MAX_EXPORT_HZ = 20.0
COMMAVIEW_MIN_EXPORT_INTERVAL_SEC = 1.0 / COMMAVIEW_MAX_EXPORT_HZ

UPSTREAM_SERVICE_ALIASES = {
  "calibration": ("extrinsicsCalibration", "liveCalibration"),
  "road_camera": ("narrowRoadCameraState", "roadCameraState"),
  "vehicle_parameters": ("vehicleParameters", "liveParameters"),
}


def _encode_json(payload: dict) -> bytes:
  if _orjson is not None:
    return _orjson.dumps(payload)
  return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _safe_float(value) -> float:
  try:
    result = float(value)
  except (TypeError, ValueError):
    return 0.0
  return result if math.isfinite(result) else 0.0


def _safe_int(value) -> int:
  try:
    if hasattr(value, "raw"):
      value = value.raw
    return int(value)
  except (TypeError, ValueError):
    return 0


def _safe_str(value) -> str:
  return "" if value is None else str(value)


def _float_list(values) -> list[float]:
  return [_safe_float(v) for v in values]


def _matrix3_list(values) -> list[float]:
  if values is None:
    return []
  try:
    if hasattr(values, "reshape"):
      values = values.reshape(-1).tolist()
    elif hasattr(values, "flatten"):
      values = values.flatten().tolist()
  except (TypeError, ValueError):
    pass
  flat = []
  try:
    iterator = list(values)
  except TypeError:
    iterator = []
  for value in iterator:
    if isinstance(value, (list, tuple)):
      flat.extend(_safe_float(v) for v in value)
    else:
      flat.append(_safe_float(value))
  return flat[:9]


def _path_dict(source) -> dict:
  return {
    "x": _float_list(getattr(source, "x", [])),
    "y": _float_list(getattr(source, "y", [])),
    "z": _float_list(getattr(source, "z", [])),
  }


def _lead_dict(source) -> dict:
  return {
    "dRel": _safe_float(getattr(source, "dRel", 0.0)),
    "yRel": _safe_float(getattr(source, "yRel", 0.0)),
    "vRel": _safe_float(getattr(source, "vRel", 0.0)),
    "aRel": _safe_float(getattr(source, "aRel", 0.0)),
    "status": bool(getattr(source, "status", False)),
  }


def _interp(value: float, xp: list[float], fp: list[float]) -> float:
  if value <= xp[0]:
    return fp[0]
  if value >= xp[-1]:
    return fp[-1]
  span = xp[-1] - xp[0]
  if span <= 0.0:
    return fp[-1]
  return fp[0] + (fp[-1] - fp[0]) * ((value - xp[0]) / span)


class _ServiceResolver:
  def __init__(self):
    self._resolved = {}

  def resolve(self, ui_state, semantic: str) -> str:
    if semantic in self._resolved:
      return self._resolved[semantic]
    aliases = UPSTREAM_SERVICE_ALIASES.get(semantic)
    if aliases is None:
      raise KeyError(f"unknown upstream service semantic: {semantic}")
    recv_frame = getattr(ui_state.sm, "recv_frame", {})
    for service in aliases:
      try:
        present = service in recv_frame
      except TypeError:
        present = False
      if present:
        self._resolved[semantic] = service
        return service
    raise KeyError(f"missing {semantic} upstream service; expected one of: {', '.join(aliases)}")

  def message(self, ui_state, semantic: str):
    return ui_state.sm[self.resolve(ui_state, semantic)]

  def recv_frame(self, ui_state, semantic: str) -> int:
    return ui_state.sm.recv_frame[self.resolve(ui_state, semantic)]

  def log_mono_time(self, ui_state, semantic: str) -> int:
    service = self.resolve(ui_state, semantic)
    try:
      return int(ui_state.sm.logMonoTime.get(service, 0) or 0)
    except (AttributeError, TypeError, ValueError):
      return 0

  @property
  def resolved(self) -> dict:
    return dict(self._resolved)


def _torque_bar_value(ui_state, service_resolver=None) -> float:
  service_resolver = service_resolver or _ServiceResolver()
  if ui_state.sm.recv_frame["controlsState"] < ui_state.started_frame:
    return 0.0

  controls_state = ui_state.sm["controlsState"]
  lateral_control = getattr(controls_state, "lateralControlState", None)
  control_mode = lateral_control.which() if lateral_control is not None and hasattr(lateral_control, "which") else ""
  if control_mode == "angleState":
    if (
      ui_state.sm.recv_frame["carState"] < ui_state.started_frame or
      ui_state.sm.recv_frame["carControl"] < ui_state.started_frame or
      service_resolver.recv_frame(ui_state, "vehicle_parameters") < ui_state.started_frame
    ):
      return 0.0

    car_state = ui_state.sm["carState"]
    car_control = ui_state.sm["carControl"]
    live_parameters = service_resolver.message(ui_state, "vehicle_parameters")
    v_ego = max(_safe_float(car_state.vEgo), 0.0)
    actual_lateral_accel = _safe_float(getattr(controls_state, "curvature", 0.0)) * v_ego ** 2
    desired_lateral_accel = _safe_float(getattr(controls_state, "desiredCurvature", 0.0)) * v_ego ** 2
    roll_compensation = _safe_float(live_parameters.roll) * ACCELERATION_DUE_TO_GRAVITY * _interp(v_ego, [5.0, 15.0], [0.0, 1.0])
    lateral_accel = actual_lateral_accel - roll_compensation
    max_lateral_accel = COMMAVIEW_DEFAULT_MAX_LAT_ACCEL
    if hasattr(ui_state, "CP") and ui_state.CP is not None:
      max_lateral_accel = _safe_float(getattr(ui_state.CP, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL)) or COMMAVIEW_DEFAULT_MAX_LAT_ACCEL
    if max_lateral_accel <= 0.0 or not bool(car_control.latActive):
      return 0.0
    return max(-1.0, min(1.0, (lateral_accel + (desired_lateral_accel - actual_lateral_accel)) / max_lateral_accel))

  if ui_state.sm.recv_frame["carOutput"] < ui_state.started_frame:
    return 0.0
  return max(-1.0, min(1.0, -_safe_float(ui_state.sm["carOutput"].actuatorsOutput.torque)))


def _status_mode_name(status) -> str:
  status_value = str(status.value if hasattr(status, "value") else status)
  return {
    "disengaged": "disengaged",
    "engaged": "engaged",
    "override": "override",
    "lat_only": "latOnly",
    "long_only": "longOnly",
  }.get(status_value, "unknown")


def _panda_states_summary(ui_state) -> tuple[bool, bool, bool]:
  ignition_line = False
  ignition_can = False
  try:
    for panda_state in ui_state.sm["pandaStates"]:
      ignition_line = ignition_line or bool(getattr(panda_state, "ignitionLine", False))
      ignition_can = ignition_can or bool(getattr(panda_state, "ignitionCan", False))
  except (KeyError, TypeError):
    pass
  return ignition_line, ignition_can, ignition_line or ignition_can


def _enum_name(value) -> str:
  if value is None:
    return ""
  raw_name = getattr(value, "name", None)
  if raw_name is not None:
    return _safe_str(raw_name).replace("_", "").lower()
  return _safe_str(value).split(".")[-1].replace("_", "").lower()


def _enum_raw(value) -> int:
  raw = getattr(value, "raw", value)
  return _safe_int(raw)


def _speed_limit_pre_active_state(ui_state) -> tuple[bool, str]:
  try:
    speed_limit = ui_state.sm["longitudinalPlanSP"].speedLimit
    assist_state = speed_limit.assist.state
  except (AttributeError, KeyError, TypeError, IndexError):
    return False, "none"

  speed_limit_pre_active = _enum_raw(assist_state) == 2 or _enum_name(assist_state) == "preactive"
  speed_limit_pre_active_icon = "none"
  if speed_limit_pre_active:
    speed_conv = COMMAVIEW_MS_TO_KPH if ui_state.is_metric else COMMAVIEW_MS_TO_MPH
    speed_limit_final_last = ui_state.sm["longitudinalPlanSP"].speedLimit.resolver.speedLimitFinalLast
    speed_limit_round = round(_safe_float(speed_limit_final_last) * speed_conv)

    try:
      v_cruise_cluster = _safe_float(ui_state.sm["carState"].vCruiseCluster)
    except (AttributeError, KeyError, TypeError, IndexError):
      v_cruise_cluster = 0.0
    try:
      controls_v_cruise = _safe_float(getattr(ui_state.sm["controlsState"], "vCruiseDEPRECATED", 0.0))
    except (AttributeError, KeyError, TypeError, IndexError):
      controls_v_cruise = 0.0

    set_speed = controls_v_cruise if v_cruise_cluster == 0.0 else v_cruise_cluster
    if not ui_state.is_metric:
      set_speed *= COMMAVIEW_KM_TO_MILE
    set_speed_round = round(set_speed)

    if set_speed_round < speed_limit_round:
      speed_limit_pre_active_icon = "up"
    elif set_speed_round > speed_limit_round:
      speed_limit_pre_active_icon = "down"

  return speed_limit_pre_active, speed_limit_pre_active_icon


class _CommaViewSocketExporter:
  def __init__(self, flavor: str, start_worker: bool = True):
    self._flavor = flavor
    self._socket_path = os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT
    self._sock = None
    self._next_connect_attempt = 0.0
    self._active_camera = "road"
    self._active_camera_override = None
    self._wide_camera_available_override = None
    self._latest_onroad_projection = None
    self._service_resolver = _ServiceResolver()
    self._pending = {}
    self._recipe_expires_at = 0
    marker = os.environ.get("COMMAVIEW_SOURCE_RECIPE_MARKER") or "/data/commaview/run/source-recipe-enabled"
    self._recipe_marker = marker
    self._recipe_next_check = 0.0
    try:
      with open(marker, encoding="ascii") as marker_file:
        self._recipe_expires_at = int(marker_file.read(32).strip())
    except (OSError, ValueError):
      pass
    if os.environ.get("COMMAVIEW_SOURCE_RECIPE") == "1":
      self._recipe_expires_at = float("inf")
    self._recipe_enabled = time.time() < self._recipe_expires_at
    self._recipe_pending = deque()
    self._recipe_sequence = 0
    self._recipe_camera = None
    self._last_recipe_anchor_time = 0.0
    self._condition = threading.Condition()
    self._stopping = False
    self._worker_busy = False
    self._worker = None
    self._worker_enabled = start_worker
    self._last_generation = {}
    self._last_offer_time = {}
    self._last_projection_offer_time = 0.0
    self._snapshot_durations_ns = deque(maxlen=512)
    self._worker_lag_ns = deque(maxlen=512)
    self._stats = {
      "offers": 0,
      "overwritten": 0,
      "rateLimited": 0,
      "snapshotExceptions": 0,
      "workerExceptions": 0,
      "socketExceptions": 0,
      "reconnects": 0,
      "sent": 0,
      "bytesSent": 0,
      "maxPending": 0,
      "recipeOverflow": 0,
      "recipeSendFailures": 0,
    }
    self._payload_builders = {
      COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX: self._ui_state_onroad_payload,
      COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX: self._selfdrive_state_payload,
      COMMAVIEW_CAR_STATE_SERVICE_INDEX: self._car_state_payload,
      COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX: self._controls_state_payload,
      COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX: self._onroad_events_payload,
      COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX: self._driver_monitoring_state_payload,
      COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX: self._driver_state_v2_payload,
      COMMAVIEW_MODEL_V2_SERVICE_INDEX: self._model_v2_payload,
      COMMAVIEW_RADAR_STATE_SERVICE_INDEX: self._radar_state_payload,
      COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX: self._live_calibration_payload,
      COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX: self._car_output_payload,
      COMMAVIEW_CAR_CONTROL_SERVICE_INDEX: self._car_control_payload,
      COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX: self._live_parameters_payload,
      COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX: self._longitudinal_plan_payload,
      COMMAVIEW_CAR_PARAMS_SERVICE_INDEX: self._car_params_payload,
      COMMAVIEW_DEVICE_STATE_SERVICE_INDEX: self._device_state_payload,
      COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX: self._road_camera_state_payload,
      COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX: self._panda_states_summary_payload,
      COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX: self._wide_road_camera_state_payload,
    }
    self._service_specs = (
      (COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX, self._ui_state_onroad_payload, ("selfdriveState", "carState", "deviceState", "pandaStates", "wideRoadCameraState")),
      (COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX, self._selfdrive_state_payload, ("selfdriveState",)),
      (COMMAVIEW_CAR_STATE_SERVICE_INDEX, self._car_state_payload, ("carState",)),
      (COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX, self._controls_state_payload, ("controlsState", "carOutput", "carControl", "@vehicle_parameters", "carState", "longitudinalPlanSP")),
      (COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX, self._onroad_events_payload, ("onroadEvents",)),
      (COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX, self._driver_monitoring_state_payload, ("driverMonitoringState",)),
      (COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX, self._driver_state_v2_payload, ("driverStateV2",)),
      (COMMAVIEW_MODEL_V2_SERVICE_INDEX, self._model_v2_payload, ("modelV2",)),
      (COMMAVIEW_RADAR_STATE_SERVICE_INDEX, self._radar_state_payload, ("radarState",)),
      (COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX, self._live_calibration_payload, ("@calibration",)),
      (COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX, self._car_output_payload, ("carOutput",)),
      (COMMAVIEW_CAR_CONTROL_SERVICE_INDEX, self._car_control_payload, ("carControl",)),
      (COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX, self._live_parameters_payload, ("@vehicle_parameters",)),
      (COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX, self._longitudinal_plan_payload, ("longitudinalPlan",)),
      (COMMAVIEW_CAR_PARAMS_SERVICE_INDEX, self._car_params_payload, ()),
      (COMMAVIEW_DEVICE_STATE_SERVICE_INDEX, self._device_state_payload, ("deviceState",)),
      (COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX, self._road_camera_state_payload, ("@road_camera",)),
      (COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX, self._panda_states_summary_payload, ("pandaStates",)),
      (COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX, self._wide_road_camera_state_payload, ("wideRoadCameraState",)),
    )

  def set_onroad_camera(self, active_camera: str, wide_camera_available: bool) -> None:
    camera = _safe_str(active_camera)
    self._active_camera_override = "wideRoad" if camera in ("wideRoad", "wide") else "road"
    self._wide_camera_available_override = bool(wide_camera_available)

  def _refresh_recipe_marker(self, now: float) -> None:
    if now < self._recipe_next_check or os.environ.get("COMMAVIEW_SOURCE_RECIPE") == "1":
      return
    self._recipe_next_check = now + 1.0
    try:
      with open(self._recipe_marker, encoding="ascii") as marker_file:
        self._recipe_expires_at = int(marker_file.read(32).strip())
    except (OSError, ValueError):
      self._recipe_expires_at = 0
    was_enabled = self._recipe_enabled
    self._recipe_enabled = time.time() < self._recipe_expires_at
    if was_enabled and not self._recipe_enabled:
      self._recipe_camera = None

  def set_onroad_projection(
    self,
    ui_state,
    active_camera: str,
    content_rect,
    video_frame_matrix,
    model_transform,
    camera_offset: float = 0.0,
  ) -> None:
    now = time.monotonic()
    self._refresh_recipe_marker(now)
    camera = _safe_str(active_camera)
    camera = "wideRoad" if camera in ("wideRoad", "wide") else "road"
    source_switch = self._recipe_enabled and camera != self._recipe_camera
    if not source_switch and self._last_projection_offer_time and now - self._last_projection_offer_time < COMMAVIEW_MIN_EXPORT_INTERVAL_SEC:
      self._stats["rateLimited"] += 1
      return
    road_service = self._service_resolver.resolve(ui_state, "road_camera")
    calibration_service = self._service_resolver.resolve(ui_state, "calibration")
    road_camera = self._service_msg(ui_state, road_service)
    wide_camera = self._service_msg(ui_state, "wideRoadCameraState")
    model = self._service_msg(ui_state, "modelV2")
    road_log_mono = self._service_log_mono(ui_state, road_service)
    wide_log_mono = self._service_log_mono(ui_state, "wideRoadCameraState")
    model_log_mono = self._service_log_mono(ui_state, "modelV2")
    live_calib_log_mono = self._service_log_mono(ui_state, calibration_service)
    self._latest_onroad_projection = {
      "valid": True,
      "camera": camera,
      "contentRect": {
        "x": _safe_float(getattr(content_rect, "x", 0.0)),
        "y": _safe_float(getattr(content_rect, "y", 0.0)),
        "width": _safe_float(getattr(content_rect, "width", 0.0)),
        "height": _safe_float(getattr(content_rect, "height", 0.0)),
      },
      "videoFrameMatrix": _matrix3_list(video_frame_matrix),
      "modelTransform": _matrix3_list(model_transform),
      "cameraOffset": [0.0, _safe_float(camera_offset), 0.0],
      "roadFrameId": _safe_int(getattr(road_camera, "frameId", 0)),
      "roadTimestampEof": _safe_int(getattr(road_camera, "timestampEof", 0)),
      "wideFrameId": _safe_int(getattr(wide_camera, "frameId", 0)),
      "wideTimestampEof": _safe_int(getattr(wide_camera, "timestampEof", 0)),
      "modelFrameId": _safe_int(getattr(model, "frameId", 0)),
      "modelTimestampEof": _safe_int(getattr(model, "timestampEof", 0)),
      "liveCalibrationLogMonoTime": live_calib_log_mono,
      "logMonoTime": max(road_log_mono, wide_log_mono, model_log_mono, live_calib_log_mono),
    }
    self._last_projection_offer_time = now
    if self._recipe_enabled:
      kind = "camera_switch" if source_switch else "anchor"
      if source_switch or now - self._last_recipe_anchor_time >= 5.0:
        self._offer_recipe_event(kind, self._latest_onroad_projection)
        self._recipe_camera = camera
        self._last_recipe_anchor_time = now
    self._offer_payload(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)

  def publish(self, ui_state) -> None:
    started_ns = time.monotonic_ns()
    for semantic in UPSTREAM_SERVICE_ALIASES:
      self._service_resolver.resolve(ui_state, semantic)
    now = time.monotonic()
    for service_index, _, dependencies in self._service_specs:
      generation = self._generation(ui_state, dependencies)
      if self._last_generation.get(service_index) == generation:
        continue
      last_offer = self._last_offer_time.get(service_index, 0.0)
      if service_index in self._last_generation and now - last_offer < COMMAVIEW_MIN_EXPORT_INTERVAL_SEC:
        self._stats["rateLimited"] += 1
        continue
      payload_fn = self._payload_builders[service_index]
      try:
        payload = payload_fn(ui_state)
      except Exception:
        self._stats["snapshotExceptions"] += 1
        continue
      self._last_generation[service_index] = generation
      self._last_offer_time[service_index] = now
      self._offer_payload(service_index, payload)
    self._snapshot_durations_ns.append(time.monotonic_ns() - started_ns)

  def _publish_json(self, service_index: int, payload_fn, ui_state) -> None:
    try:
      payload = payload_fn(ui_state)
    except Exception:
      self._stats["snapshotExceptions"] += 1
      return
    self._publish_payload(service_index, payload)

  def _publish_payload(self, service_index: int, payload: dict) -> None:
    self._offer_payload(service_index, payload)

  def _generation(self, ui_state, dependencies) -> tuple:
    generation = []
    for dependency in dependencies:
      service = self._service_resolver.resolve(ui_state, dependency[1:]) if dependency.startswith("@") else dependency
      try:
        recv_frame = int(ui_state.sm.recv_frame.get(service, -1))
      except (AttributeError, TypeError, ValueError):
        recv_frame = -1
      try:
        log_mono_time = int(ui_state.sm.logMonoTime.get(service, 0) or 0)
      except (AttributeError, TypeError, ValueError):
        log_mono_time = 0
      generation.append((service, recv_frame, log_mono_time))
    return tuple(generation)

  def _offer_payload(self, service_index: int, payload: dict) -> None:
    offered_ns = time.monotonic_ns()
    with self._condition:
      if self._worker_enabled and self._worker is None:
        self._worker = threading.Thread(target=self._worker_main, name="commaview-export", daemon=True)
        self._worker.start()
      if service_index in self._pending:
        self._stats["overwritten"] += 1
      self._pending[service_index] = (payload, offered_ns)
      self._stats["offers"] += 1
      self._stats["maxPending"] = max(self._stats["maxPending"], len(self._pending))
      self._condition.notify()

  def _offer_recipe_event(self, kind: str, projection: dict) -> None:
    # Preserve source camera changes separately from conflated live telemetry.
    # The UI thread only queues a small object; socket/JSON work stays on the worker.
    with self._condition:
      self._recipe_sequence += 1
      if len(self._recipe_pending) >= 128:
        self._stats["recipeOverflow"] += 1
        return
      if self._worker_enabled and self._worker is None:
        self._worker = threading.Thread(target=self._worker_main, name="commaview-export", daemon=True)
        self._worker.start()
      self._recipe_pending.append({
        "sequence": self._recipe_sequence, "kind": kind, "projection": projection,
      })
      self._condition.notify()

  def _pending_payload(self, service_index: int):
    with self._condition:
      pending = self._pending.get(service_index)
      return None if pending is None else pending[0]

  def _worker_main(self) -> None:
    while True:
      with self._condition:
        while not self._pending and not self._recipe_pending and not self._stopping:
          self._condition.wait()
        if self._stopping:
          break
        recipe_events = list(self._recipe_pending)
        self._recipe_pending.clear()
        batch = self._pending
        self._pending = {}
        self._worker_busy = True
      for event in recipe_events:
        try:
          if not self._send_json(COMMAVIEW_RECIPE_EVENT_SERVICE_INDEX, event):
            self._stats["recipeSendFailures"] += 1
        except Exception:
          self._stats["recipeSendFailures"] += 1
      for service_index, pending in batch.items():
        with self._condition:
          pending = self._pending.pop(service_index, pending)
        payload, offered_ns = pending
        self._worker_lag_ns.append(max(0, time.monotonic_ns() - offered_ns))
        try:
          self._send_json(service_index, payload)
        except OSError:
          self._stats["socketExceptions"] += 1
        except Exception:
          self._stats["workerExceptions"] += 1
      with self._condition:
        self._worker_busy = False
        self._condition.notify_all()
    self._close()
    with self._condition:
      self._worker_busy = False
      self._condition.notify_all()

  def wait_for_idle(self, timeout: float = 1.0) -> bool:
    deadline = time.monotonic() + timeout
    with self._condition:
      while self._pending or self._recipe_pending or self._worker_busy:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
          return False
        self._condition.wait(remaining)
      return True

  def shutdown(self, timeout: float = 1.0) -> None:
    if self._worker is None:
      return
    with self._condition:
      self._stopping = True
      self._pending.clear()
      self._recipe_pending.clear()
      self._condition.notify_all()
    self._worker.join(timeout)

  def stats(self) -> dict:
    def percentile_ms(values, percentile):
      if not values:
        return 0.0
      ordered = sorted(values)
      index = min(len(ordered) - 1, int((len(ordered) - 1) * percentile))
      return ordered[index] / 1_000_000.0

    with self._condition:
      result = dict(self._stats)
      result["pending"] = len(self._pending)
      result["recipePending"] = len(self._recipe_pending)
    result.update({
      "snapshotP50Ms": percentile_ms(self._snapshot_durations_ns, 0.50),
      "snapshotP95Ms": percentile_ms(self._snapshot_durations_ns, 0.95),
      "snapshotP99Ms": percentile_ms(self._snapshot_durations_ns, 0.99),
      "snapshotMaxMs": percentile_ms(self._snapshot_durations_ns, 1.0),
      "workerLagP95Ms": percentile_ms(self._worker_lag_ns, 0.95),
    })
    return result

  def _connect(self) -> bool:
    if self._sock is not None:
      return True
    now = time.monotonic()
    if now < self._next_connect_attempt:
      return False
    try:
      sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
      sock.settimeout(COMMAVIEW_SOCKET_TIMEOUT_SEC)
      sock.connect(self._socket_path)
    except OSError:
      try:
        sock.close()
      except (OSError, UnboundLocalError):
        pass
      self._next_connect_attempt = now + COMMAVIEW_CONNECT_RETRY_SEC
      return False
    self._sock = sock
    self._next_connect_attempt = 0.0
    self._stats["reconnects"] += 1
    return True

  def _close(self) -> None:
    if self._sock is not None:
      try:
        self._sock.close()
      except OSError:
        pass
    self._sock = None

  def _send_json(self, service_index: int, payload: dict) -> bool:
    if not self._connect():
      return False
    raw = _encode_json(payload)
    frame = bytes((COMMAVIEW_FRAME_VERSION, service_index)) + raw
    packet = struct.pack(">I", len(frame)) + frame
    try:
      self._sock.sendall(packet)
      self._stats["sent"] += 1
      self._stats["bytesSent"] += len(packet)
      return True
    except OSError:
      self._close()
      raise

  def _service_msg(self, ui_state, service: str):
    try:
      return ui_state.sm[service]
    except (KeyError, TypeError, IndexError):
      return None

  def _service_log_mono(self, ui_state, *services: str) -> int:
    values = []
    for service in services:
      try:
        values.append(int(ui_state.sm.logMonoTime.get(service, 0) or 0))
      except (AttributeError, TypeError, ValueError):
        values.append(0)
    return max(values) if values else 0

  def _wide_camera_available(self, ui_state) -> bool:
    if self._wide_camera_available_override is not None:
      return bool(self._wide_camera_available_override)
    try:
      return bool(ui_state.sm.alive["wideRoadCameraState"]) and bool(ui_state.sm.valid["wideRoadCameraState"])
    except (AttributeError, KeyError, TypeError):
      return False

  def _active_camera_name(self, ui_state) -> str:
    wide_available = self._wide_camera_available(ui_state)
    if self._active_camera_override is not None:
      self._active_camera = self._active_camera_override if wide_available else "road"
      return self._active_camera

    if not wide_available:
      self._active_camera = "road"
      return self._active_camera

    if (
      ui_state.sm.recv_frame["selfdriveState"] < ui_state.started_frame or
      ui_state.sm.recv_frame["carState"] < ui_state.started_frame
    ):
      self._active_camera = "road"
      return self._active_camera

    selfdrive_state = ui_state.sm["selfdriveState"]
    car_state = ui_state.sm["carState"]
    experimental_mode = bool(getattr(selfdrive_state, "experimentalMode", False))
    v_ego = _safe_float(getattr(car_state, "vEgo", 0.0))

    if not experimental_mode:
      target = "road"
    elif v_ego < 5.0:
      target = "wideRoad"
    elif v_ego > 10.0:
      target = "road"
    else:
      target = self._active_camera

    self._active_camera = "wideRoad" if target == "wideRoad" else "road"
    return self._active_camera

  def _ui_state_onroad_payload(self, ui_state) -> dict:
    _, _, ignition = _panda_states_summary(ui_state)
    wide_camera_available = self._wide_camera_available(ui_state)
    active_camera = self._active_camera_name(ui_state)
    return {
      "exportVersion": 1,
      "started": bool(ui_state.started),
      "ignition": bool(getattr(ui_state, "ignition", ignition)),
      "status": _status_mode_name(ui_state.status),
      "isMetric": bool(ui_state.is_metric),
      "alwaysOnDm": bool(getattr(ui_state, "always_on_dm", ui_state.params.get_bool("AlwaysOnDM"))),
      "hasLongitudinalControl": bool(getattr(ui_state, "has_longitudinal_control", False)),
      "startedFrame": _safe_int(getattr(ui_state, "started_frame", 0)),
      "startedTime": _safe_float(getattr(ui_state, "started_time", 0.0)),
      "activeCamera": active_camera,
      "wideCameraAvailable": wide_camera_available,
      "runtimeFlavor": self._flavor,
      "rainbowPathEnabled": bool(getattr(ui_state, "rainbow_path", ui_state.params.get_bool("RainbowMode"))),
      "logMonoTime": self._service_log_mono(ui_state, "deviceState", "pandaStates", "selfdriveState"),
    }

  def _selfdrive_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "enabled": False,
      "active": False,
      "engageable": False,
      "alertText1": "",
      "alertText2": "",
      "alertType": "none",
      "alertStatus": 0,
      "alertSize": 0,
      "alertHudVisual": 0,
      "experimentalMode": False,
      "logMonoTime": self._service_log_mono(ui_state, "selfdriveState"),
    }
    if ui_state.sm.recv_frame["selfdriveState"] >= ui_state.started_frame:
      selfdrive_state = ui_state.sm["selfdriveState"]
      payload.update({
        "enabled": bool(selfdrive_state.enabled),
        "active": bool(selfdrive_state.active),
        "engageable": bool(selfdrive_state.engageable),
        "alertText1": _safe_str(selfdrive_state.alertText1),
        "alertText2": _safe_str(selfdrive_state.alertText2),
        "alertType": _safe_str(selfdrive_state.alertType),
        "alertStatus": _safe_int(selfdrive_state.alertStatus),
        "alertSize": _safe_int(selfdrive_state.alertSize),
        "alertHudVisual": _safe_int(getattr(selfdrive_state, "alertHudVisual", 0)),
        "experimentalMode": bool(selfdrive_state.experimentalMode),
      })
    return payload

  def _car_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "vEgo": 0.0,
      "vEgoCluster": 0.0,
      "vCruiseCluster": 0.0,
      "standstill": False,
      "steeringAngleDeg": 0.0,
      "steeringPressed": False,
      "leftBlinker": False,
      "rightBlinker": False,
      "leftBlindspot": False,
      "rightBlindspot": False,
      "logMonoTime": self._service_log_mono(ui_state, "carState"),
    }
    if ui_state.sm.recv_frame["carState"] >= ui_state.started_frame:
      car_state = ui_state.sm["carState"]
      payload.update({
        "vEgo": _safe_float(car_state.vEgo),
        "vEgoCluster": _safe_float(car_state.vEgoCluster),
        "vCruiseCluster": _safe_float(getattr(car_state, "vCruiseCluster", 0.0)),
        "standstill": bool(car_state.standstill),
        "steeringAngleDeg": _safe_float(car_state.steeringAngleDeg),
        "steeringPressed": bool(car_state.steeringPressed),
        "leftBlinker": bool(car_state.leftBlinker),
        "rightBlinker": bool(car_state.rightBlinker),
        "leftBlindspot": bool(car_state.leftBlindspot),
        "rightBlindspot": bool(car_state.rightBlindspot),
      })
    return payload

  def _controls_state_payload(self, ui_state) -> dict:
    speed_limit_pre_active, speed_limit_pre_active_icon = _speed_limit_pre_active_state(ui_state)
    payload = {
      "exportVersion": 1,
      "enabled": False,
      "active": False,
      "engageable": False,
      "alertText1": "",
      "alertText2": "",
      "alertType": "none",
      "alertStatus": 0,
      "alertSize": 0,
      "alertHudVisual": 0,
      "experimentalMode": False,
      "speedLimitPreActive": speed_limit_pre_active,
      "speedLimitPreActiveIcon": speed_limit_pre_active_icon,
      "vCruiseDEPRECATED": 0.0,
      "lateralControlStateWhich": "",
      "curvature": 0.0,
      "desiredCurvature": 0.0,
      "torqueBarValue": 0.0,
      "logMonoTime": max(
        self._service_log_mono(ui_state, "controlsState", "carOutput", "carControl", "carState", "longitudinalPlanSP"),
        self._service_resolver.log_mono_time(ui_state, "vehicle_parameters"),
      ),
    }
    if ui_state.sm.recv_frame["controlsState"] >= ui_state.started_frame:
      controls_state = ui_state.sm["controlsState"]
      lateral_control = getattr(controls_state, "lateralControlState", None)
      control_mode = lateral_control.which() if lateral_control is not None and hasattr(lateral_control, "which") else ""
      payload.update({
        "enabled": bool(getattr(controls_state, "enabled", False)),
        "active": bool(getattr(controls_state, "active", False)),
        "engageable": bool(getattr(controls_state, "engageable", False)),
        "alertText1": _safe_str(getattr(controls_state, "alertText1", "")),
        "alertText2": _safe_str(getattr(controls_state, "alertText2", "")),
        "alertType": _safe_str(getattr(controls_state, "alertType", "none")),
        "alertStatus": _safe_int(getattr(controls_state, "alertStatus", 0)),
        "alertSize": _safe_int(getattr(controls_state, "alertSize", 0)),
        "alertHudVisual": _safe_int(getattr(controls_state, "alertHudVisual", 0)),
        "experimentalMode": bool(getattr(controls_state, "experimentalMode", False)),
        "vCruiseDEPRECATED": _safe_float(getattr(controls_state, "vCruiseDEPRECATED", 0.0)),
        "lateralControlStateWhich": _safe_str(control_mode),
        "curvature": _safe_float(getattr(controls_state, "curvature", 0.0)),
        "desiredCurvature": _safe_float(getattr(controls_state, "desiredCurvature", 0.0)),
      })
    payload["torqueBarValue"] = _safe_float(_torque_bar_value(ui_state, self._service_resolver))
    return payload

  def _onroad_events_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "events": [],
      "logMonoTime": self._service_log_mono(ui_state, "onroadEvents"),
    }
    if ui_state.sm.recv_frame["onroadEvents"] >= ui_state.started_frame:
      payload["events"] = [
        {
          "name": _safe_int(onroad_event.name.raw if hasattr(onroad_event.name, "raw") else onroad_event.name),
          "enable": bool(onroad_event.enable),
          "noEntry": bool(onroad_event.noEntry),
          "warning": bool(onroad_event.warning),
          "userDisable": bool(onroad_event.userDisable),
          "softDisable": bool(onroad_event.softDisable),
          "immediateDisable": bool(onroad_event.immediateDisable),
          "preEnable": bool(onroad_event.preEnable),
          "permanent": bool(onroad_event.permanent),
          "overrideLateral": bool(onroad_event.overrideLateral),
          "overrideLongitudinal": bool(onroad_event.overrideLongitudinal),
        }
        for onroad_event in ui_state.sm["onroadEvents"]
      ]
    return payload

  def _driver_monitoring_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "faceDetected": False,
      "isDistracted": False,
      "isRHD": False,
      "poseYawOffset": 0.0,
      "posePitchOffset": 0.0,
      "poseYawValidCount": 0,
      "posePitchValidCount": 0,
      "isLowStd": False,
      "isActiveMode": False,
      "logMonoTime": self._service_log_mono(ui_state, "driverMonitoringState"),
    }
    if ui_state.sm.recv_frame["driverMonitoringState"] >= ui_state.started_frame:
      driver_monitoring = ui_state.sm["driverMonitoringState"]
      vision_policy = getattr(driver_monitoring, "visionPolicyState", None)
      pose = getattr(vision_policy, "pose", None)
      yaw_calib = getattr(pose, "yawCalib", None)
      pitch_calib = getattr(pose, "pitchCalib", None)
      payload.update({
        "faceDetected": bool(getattr(driver_monitoring, "faceDetected", getattr(vision_policy, "faceDetected", False))),
        "isDistracted": bool(getattr(driver_monitoring, "isDistracted", getattr(vision_policy, "isDistracted", False))),
        "isRHD": bool(getattr(driver_monitoring, "isRHD", False)),
        "poseYawOffset": _safe_float(getattr(driver_monitoring, "poseYawOffset", getattr(yaw_calib, "offset", 0.0))),
        "posePitchOffset": _safe_float(getattr(driver_monitoring, "posePitchOffset", getattr(pitch_calib, "offset", 0.0))),
        "poseYawValidCount": _safe_int(getattr(driver_monitoring, "poseYawValidCount", getattr(yaw_calib, "calibratedPercent", 0))),
        "posePitchValidCount": _safe_int(getattr(driver_monitoring, "posePitchValidCount", getattr(pitch_calib, "calibratedPercent", 0))),
        "isLowStd": bool(getattr(driver_monitoring, "isLowStd", False)),
        "isActiveMode": bool(getattr(driver_monitoring, "isActiveMode", False)),
      })
    return payload

  def _driver_data_payload(self, driver_data) -> dict:
    return {
      "faceOrientation": _float_list(getattr(driver_data, "faceOrientation", [])),
      "faceOrientationStd": _float_list(getattr(driver_data, "faceOrientationStd", [])),
      "facePosition": _float_list(getattr(driver_data, "facePosition", [])),
      "facePositionStd": _float_list(getattr(driver_data, "facePositionStd", [])),
      "faceProb": _safe_float(getattr(driver_data, "faceProb", 0.0)),
      "leftEyeProb": _safe_float(getattr(driver_data, "leftEyeProb", 0.0)),
      "rightEyeProb": _safe_float(getattr(driver_data, "rightEyeProb", 0.0)),
      "leftBlinkProb": _safe_float(getattr(driver_data, "leftBlinkProb", 0.0)),
      "rightBlinkProb": _safe_float(getattr(driver_data, "rightBlinkProb", 0.0)),
      "sunglassesProb": _safe_float(getattr(driver_data, "sunglassesProb", 0.0)),
      "phoneProb": _safe_float(getattr(driver_data, "phoneProb", 0.0)),
    }

  def _driver_state_v2_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "frameId": 0,
      "modelExecutionTime": 0.0,
      "gpuExecutionTime": 0.0,
      "wheelOnRightProb": 0.0,
      "leftDriverData": self._driver_data_payload(None),
      "rightDriverData": self._driver_data_payload(None),
      "logMonoTime": self._service_log_mono(ui_state, "driverStateV2"),
    }
    if ui_state.sm.recv_frame["driverStateV2"] >= ui_state.started_frame:
      driver_state = ui_state.sm["driverStateV2"]
      payload.update({
        "frameId": _safe_int(getattr(driver_state, "frameId", 0)),
        "modelExecutionTime": _safe_float(getattr(driver_state, "modelExecutionTime", 0.0)),
        "gpuExecutionTime": _safe_float(getattr(driver_state, "gpuExecutionTime", 0.0)),
        "wheelOnRightProb": _safe_float(getattr(driver_state, "wheelOnRightProb", 0.0)),
        "leftDriverData": self._driver_data_payload(driver_state.leftDriverData),
        "rightDriverData": self._driver_data_payload(driver_state.rightDriverData),
      })
    return payload

  def _model_v2_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "frameId": 0,
      "frameIdExtra": 0,
      "frameAge": 0,
      "frameDropPerc": 0.0,
      "timestampEof": 0,
      "position": {"x": [], "y": [], "z": []},
      "laneLines": [],
      "laneLineProbs": [],
      "laneLineStds": [],
      "roadEdges": [],
      "roadEdgeStds": [],
      "meta": {
        "disengagePredictions": {
          "t": [],
          "brakeDisengageProbs": [],
          "gasDisengageProbs": [],
          "steerOverrideProbs": [],
          "brake3MetersPerSecondSquaredProbs": [],
          "brake4MetersPerSecondSquaredProbs": [],
          "brake5MetersPerSecondSquaredProbs": [],
          "gasPressProbs": [],
          "brakePressProbs": [],
        },
      },
      "acceleration": {"x": []},
      "logMonoTime": self._service_log_mono(ui_state, "modelV2"),
    }
    if ui_state.sm.recv_frame["modelV2"] >= ui_state.started_frame:
      model = ui_state.sm["modelV2"]
      disengage_predictions = getattr(getattr(model, "meta", None), "disengagePredictions", None)
      payload.update({
        "frameId": _safe_int(model.frameId),
        "frameIdExtra": _safe_int(model.frameIdExtra),
        "frameAge": _safe_int(model.frameAge),
        "frameDropPerc": _safe_float(model.frameDropPerc),
        "timestampEof": _safe_int(model.timestampEof),
        "position": _path_dict(model.position),
        "laneLines": [_path_dict(line) for line in model.laneLines],
        "laneLineProbs": _float_list(getattr(model, "laneLineProbs", [])),
        "laneLineStds": _float_list(getattr(model, "laneLineStds", [])),
        "roadEdges": [_path_dict(edge) for edge in model.roadEdges],
        "roadEdgeStds": _float_list(getattr(model, "roadEdgeStds", [])),
        "meta": {
          "disengagePredictions": {
            "t": _float_list(getattr(disengage_predictions, "t", [])),
            "brakeDisengageProbs": _float_list(getattr(disengage_predictions, "brakeDisengageProbs", [])),
            "gasDisengageProbs": _float_list(getattr(disengage_predictions, "gasDisengageProbs", [])),
            "steerOverrideProbs": _float_list(getattr(disengage_predictions, "steerOverrideProbs", [])),
            "brake3MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake3MetersPerSecondSquaredProbs", [])),
            "brake4MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake4MetersPerSecondSquaredProbs", [])),
            "brake5MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake5MetersPerSecondSquaredProbs", [])),
            "gasPressProbs": _float_list(getattr(disengage_predictions, "gasPressProbs", [])),
            "brakePressProbs": _float_list(getattr(disengage_predictions, "brakePressProbs", [])),
          },
        },
        "acceleration": {
          "x": _float_list(getattr(getattr(model, "acceleration", None), "x", [])),
        },
      })
    return payload

  def _radar_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "valid": bool(ui_state.sm.valid["radarState"]),
      "leadOne": _lead_dict(None),
      "leadTwo": _lead_dict(None),
      "logMonoTime": self._service_log_mono(ui_state, "radarState"),
    }
    if ui_state.sm.recv_frame["radarState"] >= ui_state.started_frame:
      radar_state = ui_state.sm["radarState"]
      payload.update({
        "leadOne": _lead_dict(radar_state.leadOne),
        "leadTwo": _lead_dict(radar_state.leadTwo),
      })
    return payload

  def _live_calibration_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "rpyCalib": [],
      "height": [],
      "calStatus": 0,
      "calPerc": 0,
      "wideFromDeviceEuler": [],
      "logMonoTime": self._service_resolver.log_mono_time(ui_state, "calibration"),
    }
    if self._service_resolver.recv_frame(ui_state, "calibration") >= ui_state.started_frame:
      live_calibration = self._service_resolver.message(ui_state, "calibration")
      payload.update({
        "rpyCalib": _float_list(live_calibration.rpyCalib),
        "height": _float_list(live_calibration.height),
        "calStatus": _safe_int(live_calibration.calStatus),
        "calPerc": _safe_int(live_calibration.calPerc),
        "wideFromDeviceEuler": _float_list(getattr(live_calibration, "wideFromDeviceEuler", [])),
      })
    return payload

  def _car_output_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "actuatorsOutput": {"torque": 0.0},
      "logMonoTime": self._service_log_mono(ui_state, "carOutput"),
    }
    if ui_state.sm.recv_frame["carOutput"] >= ui_state.started_frame:
      payload["actuatorsOutput"] = {
        "torque": _safe_float(ui_state.sm["carOutput"].actuatorsOutput.torque),
      }
    return payload

  def _car_control_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "latActive": False,
      "longActive": False,
      "hudControl": {
        "setSpeed": 0.0,
        "speedVisible": False,
      },
      "logMonoTime": self._service_log_mono(ui_state, "carControl"),
    }
    if ui_state.sm.recv_frame["carControl"] >= ui_state.started_frame:
      car_control = ui_state.sm["carControl"]
      hud_control = getattr(car_control, "hudControl", None)
      payload.update({
        "latActive": bool(car_control.latActive),
        "longActive": bool(car_control.longActive),
        "hudControl": {
          "setSpeed": _safe_float(getattr(hud_control, "setSpeed", 0.0)),
          "speedVisible": bool(getattr(hud_control, "speedVisible", False)),
        },
      })
    return payload

  def _live_parameters_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "roll": 0.0,
      "logMonoTime": self._service_resolver.log_mono_time(ui_state, "vehicle_parameters"),
    }
    if self._service_resolver.recv_frame(ui_state, "vehicle_parameters") >= ui_state.started_frame:
      payload["roll"] = _safe_float(self._service_resolver.message(ui_state, "vehicle_parameters").roll)
    return payload

  def _longitudinal_plan_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "allowThrottle": False,
      "logMonoTime": self._service_log_mono(ui_state, "longitudinalPlan"),
    }
    if ui_state.sm.recv_frame["longitudinalPlan"] >= ui_state.started_frame:
      payload["allowThrottle"] = bool(getattr(ui_state.sm["longitudinalPlan"], "allowThrottle", False))
    return payload

  def _car_params_payload(self, ui_state) -> dict:
    car_params = getattr(ui_state, "CP", None)
    return {
      "exportVersion": 1,
      "openpilotLongitudinalControl": bool(getattr(car_params, "openpilotLongitudinalControl", False)) if car_params is not None else False,
      "maxLateralAccel": _safe_float(getattr(car_params, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL)) if car_params is not None else COMMAVIEW_DEFAULT_MAX_LAT_ACCEL,
      "carFingerprint": _safe_str(getattr(car_params, "carFingerprint", "")) if car_params is not None else "",
      "carName": _safe_str(getattr(car_params, "carName", "")) if car_params is not None else "",
      "carVin": _safe_str(getattr(car_params, "carVin", "")) if car_params is not None else "",
      "logMonoTime": 0,
    }

  def _device_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "started": False,
      "deviceType": "",
      "logMonoTime": self._service_log_mono(ui_state, "deviceState"),
    }
    if ui_state.sm.recv_frame["deviceState"] >= 0:
      device_state = ui_state.sm["deviceState"]
      payload.update({
        "started": bool(device_state.started),
        "deviceType": _safe_str(device_state.deviceType),
      })
    return payload

  def _road_camera_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "sensor": "",
      "frameId": 0,
      "logMonoTime": self._service_resolver.log_mono_time(ui_state, "road_camera"),
    }
    if self._service_resolver.recv_frame(ui_state, "road_camera") >= 0:
      road_camera_state = self._service_resolver.message(ui_state, "road_camera")
      payload.update({
        "sensor": _safe_str(getattr(road_camera_state, "sensor", "")),
        "frameId": _safe_int(getattr(road_camera_state, "frameId", 0)),
      })
    return payload

  def _wide_road_camera_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "sensor": "",
      "frameId": 0,
      "logMonoTime": self._service_log_mono(ui_state, "wideRoadCameraState"),
    }
    if ui_state.sm.recv_frame["wideRoadCameraState"] >= 0:
      wide_camera_state = ui_state.sm["wideRoadCameraState"]
      payload.update({
        "sensor": _safe_str(getattr(wide_camera_state, "sensor", "")),
        "frameId": _safe_int(getattr(wide_camera_state, "frameId", 0)),
      })
    return payload

  def _panda_states_summary_payload(self, ui_state) -> dict:
    ignition_line, ignition_can, ignition = _panda_states_summary(ui_state)
    return {
      "exportVersion": 1,
      "ignitionLine": ignition_line,
      "ignitionCan": ignition_can,
      "ignition": ignition,
      "logMonoTime": self._service_log_mono(ui_state, "pandaStates"),
    }
