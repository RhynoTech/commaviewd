#!/usr/bin/env python3
"""Capture live comma UI timing and replayed warning/process-health messages."""

import argparse
import json
import signal
import statistics
import sys
import time
from pathlib import Path


def percentile(values, fraction):
  if not values:
    return None
  ordered = sorted(values)
  return ordered[min(len(ordered) - 1, round((len(ordered) - 1) * fraction))]


def parse_args():
  parser = argparse.ArgumentParser()
  parser.add_argument("--duration", type=int, default=180)
  parser.add_argument("--label", required=True)
  parser.add_argument("--output-dir", default="/data/commaview-bench/results")
  return parser.parse_args()


def main():
  args = parse_args()
  op_root = "/data/openpilot"
  if op_root not in sys.path:
    sys.path.insert(0, op_root)
  try:
    from openpilot.cereal import messaging
  except ImportError:
    from cereal import messaging

  output_dir = Path(args.output_dir)
  output_dir.mkdir(parents=True, exist_ok=True)
  samples_path = output_dir / f"{args.label}-ui-metrics.jsonl"
  summary_path = output_dir / f"{args.label}-ui-summary.json"

  services = ["uiDebug", "onroadEvents", "managerState", "deviceState"]
  sm = messaging.SubMaster(services)
  frame_ms = []
  cpu_ms = []
  comm_issue_samples = 0
  unhealthy_process_samples = 0
  last_events = []
  last_unhealthy = []
  stop = False

  def request_stop(_signum, _frame):
    nonlocal stop
    stop = True

  signal.signal(signal.SIGINT, request_stop)
  signal.signal(signal.SIGTERM, request_stop)
  started = time.monotonic()

  with samples_path.open("w", encoding="utf-8") as output:
    while not stop and time.monotonic() - started < args.duration:
      sm.update(1000)
      now = time.time()
      if sm.updated.get("uiDebug", False):
        frame = float(sm["uiDebug"].frameTimeMillis)
        cpu = float(sm["uiDebug"].cpuTimeMillis)
        if frame > 0:
          frame_ms.append(frame)
          cpu_ms.append(cpu)
          output.write(json.dumps({"ts": now, "type": "uiDebug", "frameTimeMs": frame,
                                   "cpuTimeMs": cpu, "fps": 1000.0 / frame}, separators=(",", ":")) + "\n")

      if sm.updated.get("onroadEvents", False):
        last_events = [str(event.name) for event in sm["onroadEvents"]]
        if "commIssue" in last_events:
          comm_issue_samples += 1
        output.write(json.dumps({"ts": now, "type": "onroadEvents", "events": last_events},
                                separators=(",", ":")) + "\n")

      if sm.updated.get("managerState", False):
        last_unhealthy = [str(proc.name) for proc in sm["managerState"].processes
                          if proc.shouldBeRunning and not proc.running]
        if last_unhealthy:
          unhealthy_process_samples += 1
        output.write(json.dumps({"ts": now, "type": "managerState", "unhealthy": last_unhealthy},
                                separators=(",", ":")) + "\n")
      output.flush()

  summary = {
    "label": args.label,
    "requestedDurationSec": args.duration,
    "actualDurationSec": round(time.monotonic() - started, 3),
    "uiSamples": len(frame_ms),
    "frameTimeMs": {
      "p50": percentile(frame_ms, 0.50),
      "p95": percentile(frame_ms, 0.95),
      "p99": percentile(frame_ms, 0.99),
      "max": max(frame_ms) if frame_ms else None,
    },
    "fps": {
      "median": (1000.0 / statistics.median(frame_ms)) if frame_ms else None,
      "minimum": (1000.0 / max(frame_ms)) if frame_ms else None,
    },
    "cpuTimeMs": {
      "p50": percentile(cpu_ms, 0.50),
      "p95": percentile(cpu_ms, 0.95),
      "max": max(cpu_ms) if cpu_ms else None,
    },
    "framesOver50Ms": sum(value > 50.0 for value in frame_ms),
    "commIssueSamples": comm_issue_samples,
    "unhealthyProcessSamples": unhealthy_process_samples,
    "lastOnroadEvents": last_events,
    "lastUnhealthyProcesses": last_unhealthy,
    "note": "onroadEvents and managerState are replayed route data; compare them with build A, not as live driving-stack proof",
  }
  summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
  print(json.dumps(summary, indent=2, sort_keys=True))
  return 0 if frame_ms else 3


if __name__ == "__main__":
  raise SystemExit(main())
