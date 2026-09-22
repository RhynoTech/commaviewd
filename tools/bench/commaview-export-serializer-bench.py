#!/usr/bin/env python3
"""Compare CommaView exporter JSON encoders with representative payloads."""

from __future__ import annotations

import argparse
import importlib.util
import json
import statistics
import sys
import time
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "comma" / "src" / "commaview_export.openpilot.py"


def load_exporter():
  sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
  car_module = types.ModuleType("opendbc.car")
  car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
  sys.modules["opendbc.car"] = car_module
  spec = importlib.util.spec_from_file_location("commaview_serializer_bench", TEMPLATE)
  if spec is None or spec.loader is None:
    raise RuntimeError(f"unable to load {TEMPLATE}")
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def representative_payload() -> dict:
  points = [round(index * 0.05, 4) for index in range(192)]
  return {
    "exportVersion": 1,
    "frameId": 123456,
    "logMonoTime": 987654321,
    "position": {"x": points, "y": points, "z": points},
    "laneLines": [{"x": points, "y": points, "z": points} for _ in range(4)],
    "laneLineProbs": [0.95, 0.9, 0.88, 0.93],
    "meta": {"engaged": True, "status": "café", "alerts": []},
  }


def measure(encoder, payload: dict, iterations: int) -> dict:
  samples = []
  size = 0
  for _ in range(iterations):
    started = time.perf_counter_ns()
    encoded = encoder(payload)
    samples.append(time.perf_counter_ns() - started)
    size = len(encoded)
  ordered = sorted(samples)
  return {
    "iterations": iterations,
    "bytes": size,
    "p50Us": round(statistics.median(ordered) / 1_000.0, 3),
    "p95Us": round(ordered[int((len(ordered) - 1) * 0.95)] / 1_000.0, 3),
    "meanUs": round(statistics.mean(ordered) / 1_000.0, 3),
  }


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--iterations", type=int, default=5_000)
  args = parser.parse_args()
  module = load_exporter()
  payload = representative_payload()
  stdlib = lambda value: json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
  results = {"stdlib": measure(stdlib, payload, args.iterations)}
  if module._orjson is not None:
    results["orjson"] = measure(module._orjson.dumps, payload, args.iterations)
    if module._orjson.dumps(payload) != stdlib(payload):
      raise RuntimeError("orjson and stdlib wire bytes differ for representative payload")
    results["speedupMean"] = round(results["stdlib"]["meanUs"] / results["orjson"]["meanUs"], 2)
  print(json.dumps(results, indent=2, sort_keys=True))
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
