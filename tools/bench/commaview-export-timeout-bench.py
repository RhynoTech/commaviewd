#!/usr/bin/env python3
"""Measure UI-offer and worker recovery behavior across socket timeouts."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import socket
import statistics
import struct
import sys
import tempfile
import threading
import time
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "comma" / "src" / "commaview_export.sunnypilot.py"


def load_exporter():
  sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
  car_module = types.ModuleType("opendbc.car")
  car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
  sys.modules["opendbc.car"] = car_module
  spec = importlib.util.spec_from_file_location(f"timeout_bench_{time.time_ns()}", TEMPLATE)
  module = importlib.util.module_from_spec(spec)
  assert spec.loader is not None
  spec.loader.exec_module(module)
  return module


def percentile(values: list[float], fraction: float) -> float:
  ordered = sorted(values)
  return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


class DrainServer:
  def __init__(self, path: str, drain: bool):
    self.path = path
    self.drain = drain
    self.frames = 0
    self.ready = threading.Event()
    self.stop = threading.Event()
    self.thread = threading.Thread(target=self.run, daemon=True)

  def start(self) -> None:
    self.thread.start()
    if not self.ready.wait(1.0):
      raise RuntimeError("server did not start")

  def run(self) -> None:
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(self.path)
    server.listen(1)
    server.settimeout(0.1)
    self.ready.set()
    try:
      while not self.stop.is_set():
        try:
          conn, _ = server.accept()
        except TimeoutError:
          continue
        with conn:
          if not self.drain:
            self.stop.wait(2.0)
            continue
          while not self.stop.is_set():
            header = conn.recv(4)
            if not header:
              break
            size = struct.unpack(">I", header)[0]
            remaining = size
            while remaining:
              chunk = conn.recv(remaining)
              if not chunk:
                break
              remaining -= len(chunk)
            if remaining == 0:
              self.frames += 1
    finally:
      server.close()

  def close(self) -> None:
    self.stop.set()
    self.thread.join(2.5)


def run_healthy(timeout_ms: int, iterations: int) -> dict:
  module = load_exporter()
  module.COMMAVIEW_SOCKET_TIMEOUT_SEC = timeout_ms / 1000.0
  with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "healthy.sock")
    server = DrainServer(path, drain=True)
    server.start()
    exporter = module._CommaViewSocketExporter("SUNNYPILOT")
    exporter._socket_path = path
    offers = []
    payload = {"schemaVersion": 2, "padding": "x" * 13_000}
    started = time.perf_counter_ns()
    for generation in range(iterations):
      payload["generation"] = generation
      before = time.perf_counter_ns()
      exporter._offer_payload(7, payload.copy())
      offers.append((time.perf_counter_ns() - before) / 1_000.0)
      if not exporter.wait_for_idle(1.0):
        raise RuntimeError("healthy worker did not become idle")
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
    stats = exporter.stats()
    exporter.shutdown()
    server.close()
  return {
    "elapsedMs": round(elapsed_ms, 3),
    "frames": server.frames,
    "offerP50Us": round(statistics.median(offers), 3),
    "offerP99Us": round(percentile(offers, 0.99), 3),
    "socketExceptions": stats["socketExceptions"],
  }


def run_stalled(timeout_ms: int) -> dict:
  module = load_exporter()
  module.COMMAVIEW_SOCKET_TIMEOUT_SEC = timeout_ms / 1000.0
  with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "stalled.sock")
    server = DrainServer(path, drain=False)
    server.start()
    exporter = module._CommaViewSocketExporter("SUNNYPILOT")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
    sock.settimeout(timeout_ms / 1000.0)
    sock.connect(path)
    exporter._sock = sock
    payload = {"schemaVersion": 2, "padding": "x" * 1_000_000}
    before_offer = time.perf_counter_ns()
    exporter._offer_payload(7, payload)
    offer_us = (time.perf_counter_ns() - before_offer) / 1_000.0
    before_wait = time.perf_counter_ns()
    idle = exporter.wait_for_idle(max(1.0, timeout_ms / 1000.0 * 4))
    recovery_ms = (time.perf_counter_ns() - before_wait) / 1_000_000.0
    stats = exporter.stats()
    exporter.shutdown()
    server.close()
  return {
    "idle": idle,
    "offerUs": round(offer_us, 3),
    "recoveryMs": round(recovery_ms, 3),
    "socketExceptions": stats["socketExceptions"],
  }


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--timeouts-ms", default="5,10,20,50,100")
  parser.add_argument("--iterations", type=int, default=100)
  parser.add_argument("--stall-trials", type=int, default=10)
  args = parser.parse_args()
  results = {}
  for timeout_ms in [int(value) for value in args.timeouts_ms.split(",")]:
    stalled = [run_stalled(timeout_ms) for _ in range(args.stall_trials)]
    results[str(timeout_ms)] = {
      "healthy": run_healthy(timeout_ms, args.iterations),
      "stalled": {
        "trials": args.stall_trials,
        "allIdle": all(result["idle"] for result in stalled),
        "allDetected": all(result["socketExceptions"] == 1 for result in stalled),
        "offerP99Us": round(percentile([result["offerUs"] for result in stalled], 0.99), 3),
        "recoveryP50Ms": round(statistics.median(result["recoveryMs"] for result in stalled), 3),
        "recoveryP95Ms": round(percentile([result["recoveryMs"] for result in stalled], 0.95), 3),
      },
    }
  print(json.dumps(results, indent=2, sort_keys=True))
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
