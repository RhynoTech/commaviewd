#!/usr/bin/env python3
"""Deterministic software engagement gate for an isolated comma4 openpilot stack."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import signal
import socket
import struct
import subprocess
import threading
import time

from opendbc.car.honda.values import CruiseButtons
from openpilot.cereal import log, messaging
from openpilot.common.params import Params
from openpilot.common.version import terms_version_sp
from openpilot.selfdrive.test.helpers import set_params_enabled
from openpilot.tools.lib.logreader import LogReader
from openpilot.tools.sim.lib.common import SimulatorState, vec3
from openpilot.tools.sim.lib.simulated_car import SimulatedCar


CRITICAL_EVENTS = {
  "canError",
  "commIssue",
  "commIssueAvgFreq",
  "commIssueTooManyBuses",
  "controlsMismatch",
  "processNotRunning",
  "relayMalfunction",
}


class ExportReceiver:
  def __init__(self, path: str):
    self.path = path
    self.frames = 0
    self.bytes = 0
    self._stop = threading.Event()
    self._thread = threading.Thread(target=self._run, daemon=True)
    self._server = None

  def start(self) -> None:
    try:
      os.unlink(self.path)
    except FileNotFoundError:
      pass
    self._thread.start()

  @staticmethod
  def _read_exact(conn: socket.socket, size: int) -> bytes | None:
    out = bytearray()
    while len(out) < size:
      chunk = conn.recv(size - len(out))
      if not chunk:
        return None
      out.extend(chunk)
    return bytes(out)

  def _run(self) -> None:
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    self._server = server
    server.settimeout(0.2)
    server.bind(self.path)
    server.listen(2)
    while not self._stop.is_set():
      try:
        conn, _ = server.accept()
      except TimeoutError:
        continue
      except OSError:
        if self._stop.is_set():
          break
        raise
      conn.settimeout(0.2)
      with conn:
        while not self._stop.is_set():
          try:
            header = self._read_exact(conn, 4)
          except TimeoutError:
            continue
          if header is None:
            break
          size = struct.unpack(">I", header)[0]
          body = self._read_exact(conn, size)
          if body is None:
            break
          self.frames += 1
          self.bytes += size + 4

  def stop(self) -> None:
    self._stop.set()
    if self._server is not None:
      self._server.close()
    self._thread.join(1.0)
    try:
      os.unlink(self.path)
    except FileNotFoundError:
      pass


def load_model_event(route_segment: str):
  for event in LogReader(route_segment):
    if event.which() == "modelV2":
      return event.as_builder()
  raise RuntimeError(f"no modelV2 event in {route_segment}")


def load_exporter(mode: str, socket_path: str):
  if mode == "baseline":
    return None
  os.environ["COMMAVIEWD_UI_EXPORT_SOCKET"] = socket_path
  path = "/data/commaview/src/commaview_export.sunnypilot.py"
  spec = importlib.util.spec_from_file_location("commaview_level2_export", path)
  module = importlib.util.module_from_spec(spec)
  assert spec.loader is not None
  spec.loader.exec_module(module)
  return module._CommaViewSocketExporter("sunnypilot")


def publish_support(pm, model_event, frame: int, state: SimulatorState) -> None:
  if frame % 5 == 0:
    model_event.logMonoTime = time.monotonic_ns()
    model_event.valid = True
    pm.send("modelV2", model_event)
    model_event.clear_write_flag()

    driver = messaging.new_message("driverStateV2", valid=True)
    driver.driverStateV2.leftDriverData.faceProb = 1.0
    driver.driverStateV2.rightDriverData.faceProb = 1.0
    pm.send("driverStateV2", driver)

  if frame % 10 == 0:
    gps = messaging.new_message("gpsLocationExternal", valid=True)
    gps.gpsLocationExternal = {
      "unixTimestampMillis": int(time.time() * 1000),
      "flags": 1,
      "horizontalAccuracy": 1.0,
      "verticalAccuracy": 1.0,
      "speedAccuracy": 0.1,
      "bearingAccuracyDeg": 0.1,
      "vNED": [0.0, state.speed, 0.0],
      "bearingDeg": 0.0,
      "latitude": 32.753,
      "longitude": -117.209,
      "altitude": 0.0,
      "speed": state.speed,
      "source": log.GpsLocationData.SensorSource.ublox,
    }
    pm.send("gpsLocationExternal", gps)

  if frame % 25 == 0:
    peripheral = messaging.new_message("peripheralState", valid=True)
    peripheral.peripheralState.pandaType = log.PandaState.PandaType.blackPanda
    peripheral.peripheralState.voltage = 12000
    pm.send("peripheralState", peripheral)

    dm = messaging.new_message("driverMonitoringState", valid=True)
    dm.driverMonitoringState.alertLevel = log.DriverMonitoringState.AlertLevel.none
    dm.driverMonitoringState.activePolicy = log.DriverMonitoringState.MonitoringPolicy.vision
    dm.driverMonitoringState.visionPolicyState.faceDetected = True
    dm.driverMonitoringState.visionPolicyState.awarenessPercent = 100
    pm.send("driverMonitoringState", dm)

  for service, field in (("accelerometer", "acceleration"), ("gyroscope", "gyroUncalibrated")):
    msg = messaging.new_message(service, valid=True)
    sensor = getattr(msg, service)
    sensor.timestamp = msg.logMonoTime
    sensor.init(field)
    getattr(sensor, field).v = [0.0, 0.0, 0.0]
    pm.send(service, msg)

  motion = messaging.new_message("deviceMotion", valid=True)
  measurement = {"x": 0.0, "y": 0.0, "z": 0.0, "xStd": 0.0, "yStd": 0.0, "zStd": 0.0, "valid": True}
  motion.deviceMotion.orientationNED = measurement
  motion.deviceMotion.velocityDevice = measurement
  motion.deviceMotion.angularVelocityDevice = measurement
  motion.deviceMotion.accelerationDevice = measurement
  motion.deviceMotion.inputsOK = True
  motion.deviceMotion.posenetOK = True
  motion.deviceMotion.sensorsOK = True
  pm.send("deviceMotion", motion)

  vehicle = messaging.new_message("vehicleParameters", valid=True)
  vehicle.vehicleParameters.posenetValid = True
  vehicle.vehicleParameters.sensorValid = True
  vehicle.vehicleParameters.steerRatio = 15.0
  vehicle.vehicleParameters.stiffnessFactor = 1.0
  vehicle.vehicleParameters.steerRatioValid = True
  vehicle.vehicleParameters.stiffnessFactorValid = True
  vehicle.vehicleParameters.angleOffsetAverageValid = True
  vehicle.vehicleParameters.angleOffsetValid = True
  vehicle.vehicleParameters.valid = True
  pm.send("vehicleParameters", vehicle)

  calibration = messaging.new_message("extrinsicsCalibration", valid=True)
  calibration.extrinsicsCalibration.calStatus = log.ExtrinsicsCalibration.Status.calibrated
  calibration.extrinsicsCalibration.validBlocks = 20
  calibration.extrinsicsCalibration.rpyCalib = [0.0, 0.0, 0.0]
  pm.send("extrinsicsCalibration", calibration)

  delay = messaging.new_message("lateralDelay", valid=True)
  delay.lateralDelay.calPerc = 100
  pm.send("lateralDelay", delay)

  torque = messaging.new_message("lateralTorqueParameters", valid=True)
  torque.lateralTorqueParameters.useParams = True
  torque.lateralTorqueParameters.calPerc = 100
  pm.send("lateralTorqueParameters", torque)


def run(args) -> dict:
  params = Params()
  set_params_enabled()
  params.put("HasAcceptedTermsSP", terms_version_sp, block=True)
  params.put_bool("AlphaLongitudinalEnabled", True, block=True)
  params.put_bool("DisableLogging", True, block=True)
  params.put_bool("NeuralNetworkLateralControl", False, block=True)

  calibration = messaging.new_message("extrinsicsCalibration")
  calibration.extrinsicsCalibration.validBlocks = 20
  calibration.extrinsicsCalibration.rpyCalib = [0.0, 0.0, 0.0]
  params.put("CalibrationParams", calibration.to_bytes(), block=True)

  delay = messaging.new_message("lateralDelay")
  delay.lateralDelay.calPerc = 100
  params.put("LiveDelay", delay.to_bytes(), block=True)

  torque = messaging.new_message("lateralTorqueParameters")
  torque.lateralTorqueParameters.useParams = True
  torque.lateralTorqueParameters.calPerc = 100
  params.put("LiveTorqueParameters", torque.to_bytes(), block=True)

  model_event = load_model_event(args.route_segment)
  pm = messaging.PubMaster([
    "modelV2", "driverStateV2", "driverMonitoringState",
    "peripheralState", "accelerometer", "gyroscope", "gpsLocationExternal",
    "deviceMotion", "vehicleParameters", "extrinsicsCalibration", "lateralDelay",
    "lateralTorqueParameters",
  ])
  sm = messaging.SubMaster([
    "selfdriveState", "onroadEvents", "managerState", "controlsState", "carControl",
  ])
  car = SimulatedCar()
  state = SimulatorState()
  state.valid = True
  state.ignition = True
  state.velocity = vec3(args.speed, 0.0, 0.0)

  receiver = ExportReceiver(args.socket_path)
  receiver.start()
  exporter = load_exporter(args.mode, args.socket_path)
  export_payload = {
    "schemaVersion": 2,
    "enabled": False,
    "active": False,
    "vEgo": args.speed,
    "model": {"position": list(range(192)), "velocity": [args.speed] * 192},
    "padding": "x" * 9000,
  }

  manager_env = os.environ.copy()
  manager_env.update({
    "PASSIVE": "0",
    "NOBOARD": "1",
    "SIMULATION": "1",
    "REPLAY": "1",
    "SKIP_FW_QUERY": "1",
    "FINGERPRINT": "HONDA_CIVIC_2022",
    "BLOCK": ",".join([
      "camerad", "modeld", "dmonitoringmodeld", "dmonitoringd", "sensord", "loggerd",
      "encoderd", "stream_encoderd", "micd", "logmessaged", "ui", "soundd",
      "manage_athenad", "manage_sunnylinkd", "qcomgpsd", "ubloxd", "pigeond",
      "modem", "tombstoned", "updated", "uploader", "webrtcd",
      "sunnylink_registration_manager", "statsd", "statsd_sp", "models_manager",
      "backup_manager", "mapd", "mapd_manager", "locationd", "locationd_llk", "paramsd",
      "calibrationd", "lagd", "torqued",
    ]),
  })
  manager_log_path = f"/data/commaview-bench/results/level2-{args.mode}-manager.log"
  manager_log = open(manager_log_path, "wb")
  manager = subprocess.Popen(
    [args.python, "openpilot/system/manager/manager.py"],
    cwd=args.openpilot_root,
    env=manager_env,
    stdout=manager_log,
    stderr=subprocess.STDOUT,
    start_new_session=True,
  )

  started = time.monotonic()
  active_started = None
  active_updates = 0
  controls_updates = 0
  car_control_enabled = 0
  engageable_seen = False
  critical_events = set()
  unhealthy_processes = set()
  button_toggle = False
  frame = 0

  try:
    while time.monotonic() - started < args.timeout:
      loop_started = time.monotonic()
      sm.update(0)
      publish_support(pm, model_event, frame, state)

      if sm.updated["selfdriveState"]:
        sd = sm["selfdriveState"]
        state.is_engaged = bool(sd.active)
        engageable_seen = engageable_seen or bool(sd.engageable)
        if sd.active:
          active_updates += 1
          if active_started is None:
            active_started = time.monotonic()
        else:
          active_updates = 0
          active_started = None

      state.cruise_button = 0
      if engageable_seen and not state.is_engaged and frame % 10 == 0:
        state.cruise_button = CruiseButtons.DECEL_SET if button_toggle else CruiseButtons.MAIN
        button_toggle = not button_toggle

      car.update(state)

      if sm.updated["controlsState"]:
        controls_updates += 1
      if sm.updated["carControl"] and sm["carControl"].enabled:
        car_control_enabled += 1
      if sm.updated["onroadEvents"]:
        for event in sm["onroadEvents"]:
          if event.name in CRITICAL_EVENTS:
            critical_events.add(str(event.name))
      if active_started is not None and sm.updated["managerState"]:
        for proc in sm["managerState"].processes:
          if proc.shouldBeRunning and not proc.running:
            unhealthy_processes.add(proc.name)

      if exporter is not None and frame % 5 == 0:
        export_payload["enabled"] = state.is_engaged
        export_payload["active"] = state.is_engaged
        exporter._offer_payload(0, export_payload)

      if active_started is not None and time.monotonic() - active_started >= args.stable_seconds:
        break

      frame += 1
      delay = 0.01 - (time.monotonic() - loop_started)
      if delay > 0:
        time.sleep(delay)
  finally:
    if exporter is not None:
      exporter.shutdown()
    receiver.stop()
    os.killpg(manager.pid, signal.SIGINT)
    try:
      manager.wait(20)
    except subprocess.TimeoutExpired:
      os.killpg(manager.pid, signal.SIGKILL)
      manager.wait()
    manager_log.close()

  elapsed = time.monotonic() - started
  exporter_stats = {} if exporter is None else exporter.stats()
  result = {
    "mode": args.mode,
    "engageableSeen": engageable_seen,
    "engaged": active_started is not None,
    "stableSeconds": 0.0 if active_started is None else min(args.stable_seconds, time.monotonic() - active_started),
    "elapsedSeconds": elapsed,
    "activeUpdates": active_updates,
    "controlsUpdates": controls_updates,
    "carControlEnabledUpdates": car_control_enabled,
    "criticalEvents": sorted(critical_events),
    "unhealthyProcesses": sorted(unhealthy_processes),
    "exportFrames": receiver.frames,
    "exportBytes": receiver.bytes,
    "exporterStats": exporter_stats,
    "managerExitCode": manager.returncode,
  }
  result["passed"] = all([
    result["engageableSeen"],
    result["engaged"],
    result["stableSeconds"] >= args.stable_seconds,
    result["activeUpdates"] >= int(args.stable_seconds * 70),
    result["controlsUpdates"] >= int(args.stable_seconds * 70),
    result["carControlEnabledUpdates"] >= int(args.stable_seconds * 70),
    not result["criticalEvents"],
    not result["unhealthyProcesses"],
    args.mode == "baseline" or result["exportFrames"] > 0,
  ])
  return result


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--mode", choices=("baseline", "thread"), required=True)
  parser.add_argument("--openpilot-root", default="/data/openpilot-dev")
  parser.add_argument("--python", default="/data/openpilot-dev/.venv/bin/python")
  parser.add_argument("--route-segment", default="/data/media/0/realdata/00000501--5ca603d14f--0/rlog.zst")
  parser.add_argument("--socket-path", required=True)
  parser.add_argument("--timeout", type=float, default=60.0)
  parser.add_argument("--stable-seconds", type=float, default=5.0)
  parser.add_argument("--speed", type=float, default=15.0)
  args = parser.parse_args()
  result = run(args)
  print(json.dumps(result, indent=2, sort_keys=True))
  return 0 if result["passed"] else 1


if __name__ == "__main__":
  raise SystemExit(main())
