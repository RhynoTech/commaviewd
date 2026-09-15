#!/usr/bin/env python3
"""Sanitize an openpilot/sunnypilot route into a CommaView contribution bundle.

Runs on the comma device (openpilot at /data/openpilot) or on a PC with an
openpilot checkout on PYTHONPATH. Whitelist-only: every service that is not
explicitly preserved is dropped, so new upstream services are excluded by
default. The bundle this writes is the only artifact intended to leave the
user's machine.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from collections import Counter
from pathlib import Path

DEVICE_OPENPILOT = Path("/data/openpilot")
DEVICE_PARAMS = Path("/data/params/d")

# Services CommaView's UI export templates actually consume.
# Source of truth: comma/src/commaview_export.openpilot.py and
# comma/src/commaview_export.sunnypilot.py (union across both flavors).
PRESERVE_SERVICES = (
  "carState",
  "carControl",
  "carOutput",
  "controlsState",
  "selfdriveState",
  "modelV2",
  "radarState",
  "longitudinalPlan",
  "longitudinalPlanSP",  # sunnypilot-only template read (speed limit resolver)
  "liveCalibration",
  "liveParameters",      # both templates read it (steering/roll/recv_frame gating)
  "onroadEvents",        # both templates read it (alert/event list)
  "deviceState",
  "pandaStates",
  "roadCameraState",
  "wideRoadCameraState",
  "carParams",           # preserved with VIN masked
)

# Interior-camera derived. Opt-in only: head pose, eye state, distraction.
DRIVER_SERVICES = (
  "driverStateV2",
  "driverMonitoringState",
)

# Camera files that may be opted into, by flag token.
VIDEO_FILES = {
  "road": "fcamera.hevc",
  "wide": "ecamera.hevc",
  "qcamera": "qcamera.ts",
}

# Driver-facing interior video. No flag, no token, never copied.
FORBIDDEN_FILES = ("dcamera.hevc",)

RLOG_NAMES = ("rlog.zst", "rlog.bz2", "rlog")
SEGMENT_RE = re.compile(r"^(?P<route>.+)--(?P<num>\d+)$")


def fail(message: str) -> None:
  print(f"ERROR: {message}", file=sys.stderr)
  raise SystemExit(1)


def bootstrap_openpilot() -> None:
  """Make `openpilot.tools.lib` importable on device and on a PC checkout."""
  env_root = os.environ.get("OPENPILOT_ROOT")

  # An explicit OPENPILOT_ROOT wins over whatever happens to be importable:
  # reading a sunnypilot route with a vanilla openpilot schema silently
  # renames fork services to customReservedN and they get dropped.
  if not env_root:
    try:
      import openpilot.tools.lib.logreader  # noqa: F401
      return
    except ImportError:
      pass

  candidates = [DEVICE_OPENPILOT]
  if env_root:
    candidates.insert(0, Path(env_root))

  for root in candidates:
    if (root / "tools" / "lib" / "logreader.py").is_file():
      sys.path.insert(0, str(root.parent if root.name == "openpilot" else root))
      sys.path.insert(0, str(root))
      try:
        import openpilot.tools.lib.logreader  # noqa: F401
        return
      except ImportError:
        continue

  fail(
    "could not import openpilot.tools.lib.logreader.\n"
    "  On a comma device this script expects /data/openpilot.\n"
    "  On a PC: PYTHONPATH=/path/to/openpilot python3 sanitize_route_for_contribution.py ...\n"
    "  (or set OPENPILOT_ROOT=/path/to/openpilot)"
  )


# ---------------------------------------------------------------- sanitizing

def sanitize_vin(vin: str) -> str:
  # Last 6 chars of a VIN are the serial number; mask them.
  # Same approach as openpilot/tools/lib/sanitizer.py.
  VIN_SENSITIVE = 6
  return vin[:-VIN_SENSITIVE] + "X" * VIN_SENSITIVE


def sanitize_msg(msg):
  if msg.which() == "carParams":
    builder = msg.as_builder()
    builder.carParams.carVin = sanitize_vin(builder.carParams.carVin)
    return builder.as_reader()
  return msg


# ----------------------------------------------------------- build identity

def _read_param(name: str) -> str | None:
  path = DEVICE_PARAMS / name
  try:
    value = path.read_bytes().decode("utf-8", "replace").strip()
  except OSError:
    return None
  return value or None


def detect_flavor(remote: str | None) -> str:
  if not remote:
    return "unknown"
  low = remote.lower()
  if "sunnypilot" in low:
    return "sunnypilot"
  if "commaai/openpilot" in low:
    return "openpilot"
  return "fork"


def identity_from_params() -> dict:
  """Build identity from /data/params/d/* (device path). No IDs, ever."""
  if not DEVICE_PARAMS.is_dir():
    return {}
  out = {
    "gitRemote": _read_param("GitRemote"),
    "gitBranch": _read_param("GitBranch"),
    "gitCommit": _read_param("GitCommit"),
    "gitCommitDate": _read_param("GitCommitDate"),
    "version": _read_param("Version"),
  }
  diff = _read_param("GitDiff")
  if diff is not None:
    out["dirty"] = bool(diff)
  out = {k: v for k, v in out.items() if v is not None}
  if out:
    out["identitySource"] = "device params (/data/params/d)"
  return out


def identity_from_rlog(rlog_path: Path, LogReader) -> dict:
  """Fallback: pull only build fields out of initData. Never emit initData."""
  out: dict = {}
  try:
    for msg in LogReader(str(rlog_path)):
      if msg.which() != "initData":
        continue
      d = msg.initData
      out = {
        "gitRemote": str(d.gitRemote) or None,
        "gitBranch": str(d.gitBranch) or None,
        "gitCommit": str(d.gitCommit) or None,
        "gitCommitDate": str(d.gitCommitDate).strip("'") or None,
        "version": str(d.version) or None,
        "dirty": bool(d.dirty),
        "deviceModel": str(d.deviceType),
        "identitySource": "route rlog initData (build fields only)",
      }
      break
  except Exception as exc:  # noqa: BLE001 - identity is best effort
    out = {"identityError": f"{type(exc).__name__}: {exc}"}
  return {k: v for k, v in out.items() if v is not None}


def device_model_from_params() -> str | None:
  for candidate in ("/sys/firmware/devicetree/base/model",):
    try:
      return Path(candidate).read_text(errors="replace").strip("\x00 \n")
    except OSError:
      continue
  return None


def build_manifest(rlog_path: Path, LogReader) -> dict:
  identity = identity_from_rlog(rlog_path, LogReader)
  params_identity = identity_from_params()
  if params_identity:
    sources = [s for s in (params_identity.get("identitySource"), identity.get("identitySource")) if s]
    identity.update(params_identity)
    identity["identitySource"] = " + ".join(sources)

  model = device_model_from_params()
  if model:
    identity["deviceModel"] = model

  identity["flavor"] = detect_flavor(identity.get("gitRemote"))
  identity["clean"] = (not identity["dirty"]) if "dirty" in identity else None
  if identity.get("clean") is None:
    identity["cleanNote"] = "clean/dirty could not be determined"
  return identity


# ------------------------------------------------------------ route walking

def find_segments(root: Path) -> dict[str, dict[int, Path]]:
  """Map route name -> {segment number: dir}. Accepts a route dir or a parent."""
  routes: dict[str, dict[int, Path]] = {}

  def consider(d: Path) -> None:
    m = SEGMENT_RE.match(d.name)
    if not m:
      return
    if not any((d / n).is_file() for n in RLOG_NAMES):
      return
    routes.setdefault(m.group("route"), {})[int(m.group("num"))] = d

  if SEGMENT_RE.match(root.name) and any((root / n).is_file() for n in RLOG_NAMES):
    consider(root)
    return routes

  for child in sorted(root.iterdir()):
    if not child.is_dir():
      continue
    consider(child)
    if not SEGMENT_RE.match(child.name):
      for grandchild in sorted(child.iterdir()):
        if grandchild.is_dir():
          consider(grandchild)
  return routes


def parse_segment_spec(spec: str) -> set[int]:
  out: set[int] = set()
  for part in spec.split(","):
    part = part.strip()
    if not part:
      continue
    if "-" in part.lstrip("-"):
      lo, _, hi = part.partition("-")
      out.update(range(int(lo), int(hi) + 1))
    else:
      out.add(int(part))
  return out


def rlog_in(seg: Path) -> Path:
  for name in RLOG_NAMES:
    p = seg / name
    if p.is_file():
      return p
  fail(f"no rlog in {seg}")
  raise AssertionError  # unreachable


# --------------------------------------------------------------------- main

def human(n: int) -> str:
  for unit in ("B", "KiB", "MiB", "GiB"):
    if n < 1024 or unit == "GiB":
      return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
    n /= 1024.0
  raise AssertionError


def main() -> int:
  ap = argparse.ArgumentParser(
    description="Sanitize a drive route into a CommaView contribution bundle.",
    formatter_class=argparse.RawDescriptionHelpFormatter,
    epilog=(
      "Whitelist-only. Location, GPS, navigation, logs, driver-facing camera,\n"
      "dongle ID and every other unlisted service are dropped, not scrubbed.\n"
    ),
  )
  ap.add_argument("route", type=Path, help="route directory, a parent of route dirs, or one segment dir")
  ap.add_argument("-o", "--output", type=Path, default=Path("."), help="where to write the bundle (default: cwd)")
  ap.add_argument("--segments", help="segment numbers to include, e.g. 118,119 or 118-121")
  ap.add_argument("--route-name", help="route name when the input dir holds more than one route")
  ap.add_argument("--include-video", default="",
                  help="comma list of road,wide,qcamera. Default: no video at all. "
                       "Driver-facing video is never available.")
  ap.add_argument("--include-driver-monitoring", action="store_true",
                  help="opt in to driverStateV2 + driverMonitoringState (interior-camera derived)")
  ap.add_argument("--allow-endpoints", action="store_true",
                  help="allow the route's first and last segment (usually home/work)")
  ap.add_argument("--tar", action="store_true", help="also write a .tar.gz of the bundle")
  ap.add_argument("--force", action="store_true", help="overwrite an existing bundle directory")
  args = ap.parse_args()

  bootstrap_openpilot()
  from openpilot.tools.lib.logreader import LogReader, save_log

  root = args.route.expanduser().resolve()
  if not root.is_dir():
    fail(f"not a directory: {root}")

  routes = find_segments(root)
  if not routes:
    fail(f"no segments with an rlog found under {root}")
  if args.route_name:
    if args.route_name not in routes:
      fail(f"route {args.route_name!r} not found; available: {', '.join(sorted(routes))}")
    route_name = args.route_name
  elif len(routes) > 1:
    fail(f"{len(routes)} routes found; pick one with --route-name: {', '.join(sorted(routes))}")
  else:
    route_name = next(iter(routes))

  available = routes[route_name]
  nums = sorted(available)
  first, last = nums[0], nums[-1]

  wanted = sorted(parse_segment_spec(args.segments) & set(nums)) if args.segments else list(nums)
  if args.segments:
    missing = sorted(parse_segment_spec(args.segments) - set(nums))
    if missing:
      print(f"note: requested segments not on disk, skipped: {missing}")
  if not wanted:
    fail("no requested segments exist on disk")

  refused: list[int] = []
  if not args.allow_endpoints:
    refused = [n for n in wanted if n in (first, last)]
    wanted = [n for n in wanted if n not in (first, last)]
    if refused:
      print(
        "Refusing the route's first and last segment by default: "
        f"{refused}\n"
        "  A drive normally starts where you live and ends where you were going.\n"
        "  Those two segments are the ones that identify you. Pass --allow-endpoints\n"
        "  only if you know the start and end of this drive are not sensitive.\n"
      )
  if not wanted:
    fail("nothing left to export after endpoint protection (use --allow-endpoints or pick middle segments)")

  video_tokens = [t.strip() for t in args.include_video.split(",") if t.strip()]
  bad = [t for t in video_tokens if t not in VIDEO_FILES]
  if bad:
    fail(f"unknown --include-video value(s): {bad}. Valid: {', '.join(VIDEO_FILES)}")
  if any(t in ("driver", "dcamera", "interior") for t in video_tokens):
    fail("driver-facing video is never exportable")

  preserve = list(PRESERVE_SERVICES)
  if args.include_driver_monitoring:
    preserve += list(DRIVER_SERVICES)
  preserve_set = set(preserve)

  stamp = time.strftime("%Y%m%dT%H%M%S")
  bundle = (args.output.expanduser().resolve() / f"commaview-donation-{route_name}-{stamp}")
  if bundle.exists():
    if not args.force:
      fail(f"{bundle} already exists (use --force)")
    shutil.rmtree(bundle)
  bundle.mkdir(parents=True)

  kept: Counter = Counter()
  dropped: Counter = Counter()
  vin_masked = 0
  vin_examples: list[str] = []
  files_written: list[tuple[str, int]] = []

  manifest_identity = build_manifest(rlog_in(available[wanted[0]]), LogReader)

  for num in wanted:
    seg = available[num]
    out_seg = bundle / seg.name
    out_seg.mkdir()

    src_rlog = rlog_in(seg)
    clean_msgs = []
    for msg in LogReader(str(src_rlog)):
      which = msg.which()
      if which not in preserve_set:
        dropped[which] += 1
        continue
      kept[which] += 1
      if which == "carParams":
        before = str(msg.carParams.carVin)
        msg = sanitize_msg(msg)
        after = str(msg.carParams.carVin)
        if after != before:
          vin_masked += 1
          if after not in vin_examples:
            vin_examples.append(after)
      clean_msgs.append(msg)

    dest = out_seg / "rlog.zst"
    save_log(str(dest), clean_msgs, compress=True)
    files_written.append((str(dest.relative_to(bundle)), dest.stat().st_size))

    for token in video_tokens:
      name = VIDEO_FILES[token]
      src = seg / name
      if not src.is_file():
        print(f"note: {seg.name} has no {name}, skipped")
        continue
      dst = out_seg / name
      shutil.copy2(src, dst)
      files_written.append((str(dst.relative_to(bundle)), dst.stat().st_size))

    for forbidden in FORBIDDEN_FILES:
      assert not (out_seg / forbidden).exists(), "driver camera leaked into bundle"

  manifest = {
    "bundleFormat": "commaview-contribution/1",
    "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "routeName": route_name,
    "segments": wanted,
    "segmentsRefusedAsEndpoints": refused,
    "build": manifest_identity,
    "preservedServices": sorted(preserve_set),
    "driverMonitoringIncluded": bool(args.include_driver_monitoring),
    "videoIncluded": video_tokens,
    "vinMaskedMessages": vin_masked,
    "note": "No dongle ID, serial, account identifier, GPS, navigation or log text is present by design.",
  }
  (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

  total = sum(size for _, size in files_written) + (bundle / "manifest.json").stat().st_size

  lines: list[str] = []
  w = lines.append
  w("=" * 72)
  w("CommaView contribution receipt - this is exactly what you are sending")
  w("=" * 72)
  w(f"Route:    {route_name}")
  w(f"Segments: {wanted}")
  if refused:
    w(f"Refused as route endpoints (home/work risk): {refused}")
  b = manifest_identity
  w("")
  w("Build identity (no device or account identifiers):")
  w(f"  flavor        {b.get('flavor', 'unknown')}")
  w(f"  remote        {b.get('gitRemote', 'unknown')}")
  w(f"  branch        {b.get('gitBranch', 'unknown')}")
  w(f"  commit        {b.get('gitCommit', 'unknown')}")
  w(f"  commit date   {b.get('gitCommitDate', 'unknown')}")
  w(f"  version       {b.get('version', 'unknown')}")
  w(f"  device model  {b.get('deviceModel', 'unknown')}")
  w(f"  working tree  {'clean' if b.get('clean') else 'dirty' if b.get('clean') is False else 'unknown'}")
  w(f"  source        {b.get('identitySource', 'unknown')}")
  w("")
  w(f"KEPT services ({len(kept)}):")
  for name in sorted(kept):
    w(f"  + {name:<26} {kept[name]:>7} messages")
  w("")
  w(f"DROPPED services ({len(dropped)}) - removed, not sent:")
  for name in sorted(dropped):
    w(f"  - {name:<26} {dropped[name]:>7} messages")
  w("")
  if vin_masked:
    w(f"VIN masking: applied to {vin_masked} carParams message(s); VIN now reads {', '.join(vin_examples)}")
  else:
    w("VIN masking: no carParams message contained a VIN")
  w("")
  w("Driver-facing camera (dcamera.hevc): never included, not available by any flag")
  if args.include_driver_monitoring:
    w("Driver monitoring: INCLUDED by your flag (head pose, eye state, distraction)")
  else:
    w("Driver monitoring: excluded (default)")
  if video_tokens:
    w(f"Video INCLUDED by your flag: {', '.join(video_tokens)}")
    w("  Road/wide/qcamera video still shows where you drove. It cannot be anonymized.")
  else:
    w("Video: none included (default)")
  w("")
  w("Files in bundle:")
  w(f"  {'manifest.json':<48} {human((bundle / 'manifest.json').stat().st_size)}")
  for rel, size in files_written:
    w(f"  {rel:<48} {human(size)}")
  w("")
  w(f"Total bundle size: {human(total)}")
  w(f"Bundle path:       {bundle}")
  w("=" * 72)

  receipt = "\n".join(lines) + "\n"
  (bundle / "receipt.txt").write_text(receipt)
  print(receipt)

  if args.tar:
    tar_path = bundle.with_suffix(".tar.gz")
    with tarfile.open(tar_path, "w:gz") as tf:
      tf.add(bundle, arcname=bundle.name)
    print(f"Archive: {tar_path} ({human(tar_path.stat().st_size)})")

  return 0


if __name__ == "__main__":
  raise SystemExit(main())
