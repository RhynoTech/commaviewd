#!/usr/bin/env python3
"""CommaView drive stats: what this comma drove, from what it already keeps.

A short run, started by the control service; nothing here subscribes to openpilot's messages or
stays running between drives.

  after-drive  Read the drive's own logs (openpilot writes a qlog for every minute of driving, with
               the car's speed ten times a second and its GPS once a second) for its distance and,
               when location is on and the control service didn't catch it, where it ended. Then
               refresh comma's totals.
  totals       Refresh comma's past-week and all-time totals for this comma: sunnypilot's cached
               copy (ApiCache_DriveStats) when it has one, otherwise one request to comma made with
               the comma's own identity, as sunnypilot's Trips page does.
  position     While a drive goes on and location is on: where the car was at the end of the drive's
               last finished log minute. The fallback when the comma's GPS can't be read directly.

Files (under ROOT unless noted): data/drives.json, data/drive-stats.json, and only while location is
on data/location-last.json and LIVE_DIR/location-log.json. Positions are never logged.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

ROOT = os.environ.get("COMMAVIEW_DRIVE_ROOT", "/data/commaview")
LIVE_DIR = os.environ.get("COMMAVIEW_DRIVE_LIVE_DIR", "/dev/shm/commaview")
PARAMS_DIR = os.environ.get("COMMAVIEW_DRIVE_PARAMS_DIR", "/data/params/d")
LOG_ROOT = os.environ.get("COMMAVIEW_DRIVE_LOG_ROOT", "/data/media/0/realdata")

DRIVES_FILE = os.path.join(ROOT, "data", "drives.json")
STATS_FILE = os.path.join(ROOT, "data", "drive-stats.json")
LOCATION_CONFIG_FILE = os.path.join(ROOT, "config", "location.json")
LOCATION_LAST_FILE = os.path.join(ROOT, "data", "location-last.json")
LOCATION_LOG_FILE = os.path.join(LIVE_DIR, "location-log.json")

KEEP_DAYS = 62
DAY_MS = 86_400_000
# A clock before 2024 hasn't been set yet (no GPS or network time since boot).
VALID_WALL_MS = 1_704_067_200_000
# Speed samples further apart than this aren't joined, so a gap in the log never counts as distance.
MAX_SAMPLE_GAP_S = 1.0
GPS_EVENTS = ("gpsLocationExternal", "gpsLocation")


def log(message: str) -> None:
  print(f"commaview-drive-stats: {message}", flush=True)


def valid_wall(ms) -> bool:
  return isinstance(ms, int) and not isinstance(ms, bool) and ms >= VALID_WALL_MS


def read_json(path: str):
  try:
    with open(path, encoding="utf-8") as f:
      return json.load(f)
  except (OSError, ValueError):
    return None


def write_json_atomic(path: str, value) -> None:
  os.makedirs(os.path.dirname(path), exist_ok=True)
  tmp = f"{path}.tmp.{os.getpid()}"
  with open(tmp, "w", encoding="utf-8") as f:
    json.dump(value, f, separators=(",", ":"))
  os.replace(tmp, path)


def read_param(key: str) -> str:
  try:
    with open(os.path.join(PARAMS_DIR, key), encoding="utf-8", errors="replace") as f:
      return f.read().strip("\x00\r\n\t ")
  except OSError:
    return ""


def location_enabled() -> bool:
  config = read_json(LOCATION_CONFIG_FILE)
  return isinstance(config, dict) and config.get("enabled") is True


# ---------------------------------------------------------------- The drive's own logs

def route_segments(route: str, log_root: str = None) -> list:
  """The route's segment folders on the comma, in order: <route>--0, <route>--1, ..."""
  log_root = log_root or LOG_ROOT
  if not route or "/" in route or route.startswith("."):
    return []
  pattern = re.compile(re.escape(route) + r"--(\d+)$")
  try:
    names = os.listdir(log_root)
  except OSError:
    return []
  numbered = sorted((int(m.group(1)), name) for name in names if (m := pattern.match(name)))
  return [os.path.join(log_root, name) for _, name in numbered]


def import_log():
  try:
    from cereal import log as capnp_log  # openpilot's layout, as openpilot itself imports it
  except ImportError:
    from openpilot.cereal import log as capnp_log  # sunnypilot's newer package layout
  return capnp_log


def read_qlog(path: str):
  """Events from one qlog.zst, as (which, logMonoTime, message). A log cut short keeps what it has."""
  import zstandard
  capnp_log = import_log()
  chunks = []
  try:
    with open(path, "rb") as f:
      try:
        reader = zstandard.ZstdDecompressor().stream_reader(f, read_across_frames=True)
      except TypeError:
        reader = zstandard.ZstdDecompressor().stream_reader(f)
      while True:
        chunk = reader.read(1 << 20)
        if not chunk:
          break
        chunks.append(chunk)
  except (OSError, zstandard.ZstdError):
    pass
  try:
    for event in capnp_log.Event.read_multiple_bytes(b"".join(chunks)):
      which = event.which()
      yield which, event.logMonoTime, getattr(event, which)
  except Exception:
    return  # the last message of a log cut short


def fix_from_gps(gps):
  """A GPS message as the position this comma reports, or None without a usable fix."""
  try:
    try:
      has_fix = bool(gps.hasFix)
    except Exception:
      has_fix = bool(int(gps.flags) % 2)
    lat, lon = float(gps.latitude), float(gps.longitude)
    if not has_fix or not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0) or (abs(lat) < 1e-6 and abs(lon) < 1e-6):
      return None
    return {
      "lat": round(lat, 6),
      "lon": round(lon, 6),
      "accuracyM": round(float(gps.horizontalAccuracy), 1),
      "bearingDeg": round(float(gps.bearingDeg), 1),
      "speedMs": round(float(gps.speed), 2),
      "fixMs": int(gps.unixTimestampMillis),
    }
  except Exception:
    return None


class DriveSummary:
  """Distance from the logged speed, and the last GPS fix, over a drive's events in order."""

  def __init__(self):
    self.distance_m = 0.0
    self.last_speed = None
    self.last_mono = None
    self.last_fix = None

  def add(self, which: str, mono_ns: int, message) -> None:
    if which == "carState":
      speed, mono = float(message.vEgo), mono_ns / 1e9
      if self.last_mono is not None and mono <= self.last_mono:
        return
      if self.last_mono is not None and self.last_speed is not None and 0.0 < mono - self.last_mono <= MAX_SAMPLE_GAP_S:
        self.distance_m += max(0.0, (self.last_speed + speed) / 2.0) * (mono - self.last_mono)
      self.last_speed, self.last_mono = speed, mono
    elif which in GPS_EVENTS:
      fix = fix_from_gps(message)
      if fix is not None:
        self.last_fix = fix


def summarize_route(route: str, reader=read_qlog) -> DriveSummary:
  summary = DriveSummary()
  for segment in route_segments(route):
    for which, mono, message in reader(os.path.join(segment, "qlog.zst")):
      summary.add(which, mono, message)
  return summary


# ---------------------------------------------------------------- The drive list

class DriveLedger:
  """The drives this comma has made since it started keeping them; phones group them into days."""

  def __init__(self, saved=None):
    saved = saved if isinstance(saved, dict) else {}
    self.since_ms = saved.get("sinceMs") if valid_wall(saved.get("sinceMs")) else None
    drives = saved.get("drives") if isinstance(saved.get("drives"), list) else []
    self.drives = [d for d in drives if isinstance(d, dict) and valid_wall(d.get("startMs")) and valid_wall(d.get("endMs"))]

  def add(self, drive: dict, now_ms: int) -> None:
    if self.since_ms is None and valid_wall(now_ms):
      self.since_ms = min(now_ms, drive["startMs"])
    # The same drive summarized again replaces itself.
    self.drives = [d for d in self.drives if not (drive.get("route") and d.get("route") == drive["route"])]
    self.drives.append(drive)
    self.drives.sort(key=lambda d: d["startMs"])

  def prune(self, now_ms: int) -> None:
    if valid_wall(now_ms):
      oldest = now_ms - KEEP_DAYS * DAY_MS
      self.drives = [d for d in self.drives if d["endMs"] >= oldest]

  def to_json(self) -> dict:
    return {"version": 1, "sinceMs": self.since_ms, "drives": self.drives}


def place_drive(start_ms: int, end_ms: int, duration_s: int, now_ms: int):
  """The drive's start and end on the wall clock, even when the clock was set during the drive."""
  if valid_wall(end_ms):
    end = end_ms
  elif valid_wall(now_ms):
    end = now_ms
  else:
    return None
  start = start_ms if valid_wall(start_ms) and start_ms <= end else end - duration_s * 1000
  return start, end


def after_drive(route: str, start_ms: int, end_ms: int, duration_s: int, now_ms: int, reader=read_qlog) -> dict | None:
  placed = place_drive(start_ms, end_ms, duration_s, now_ms)
  if placed is None:
    log("the clock isn't set; this drive can't be placed")
    return None
  summary = summarize_route(route, reader)
  drive = {"startMs": placed[0], "endMs": placed[1], "durationS": int(duration_s), "distanceM": int(round(summary.distance_m))}
  if route:
    drive["route"] = route
  ledger = DriveLedger(read_json(DRIVES_FILE))
  ledger.add(drive, now_ms)
  ledger.prune(now_ms)
  write_json_atomic(DRIVES_FILE, ledger.to_json())
  if summary.last_fix is not None and location_enabled():
    saved = read_json(LOCATION_LAST_FILE)
    # The control service keeps where the drive ended from the comma's own GPS; the log fills in
    # only when it couldn't.
    from_comma = isinstance(saved, dict) and valid_wall(saved.get("endedMs")) and saved["endedMs"] >= placed[0]
    if not from_comma:
      write_json_atomic(LOCATION_LAST_FILE, dict(summary.last_fix, endedMs=placed[1], source="log"))
  try:
    os.remove(LOCATION_LOG_FILE)  # the drive is over; its position now lives in location-last
  except OSError:
    pass
  return drive


def latest_position(route: str, reader=read_qlog):
  """Where the car was at the end of the drive's last finished minute (the newest log is still open)."""
  segments = route_segments(route)
  for segment in reversed(segments[:-1]):
    summary = DriveSummary()
    for which, mono, message in reader(os.path.join(segment, "qlog.zst")):
      summary.add(which, mono, message)
    if summary.last_fix is not None:
      return dict(summary.last_fix, segment=os.path.basename(segment), source="log")
  return None


def position(route: str, reader=read_qlog):
  if not location_enabled():
    return None
  fix = latest_position(route, reader)
  if fix is not None and location_enabled():
    write_json_atomic(LOCATION_LOG_FILE, fix)
  return fix


# ---------------------------------------------------------------- comma's totals

def normalize_stats(data) -> dict:
  """comma's /v1.1/devices/:dongle_id/stats body as {all, week}: routes, miles, minutes."""
  def part(value):
    value = value if isinstance(value, dict) else {}
    return {
      "routes": int(value.get("routes") or 0),
      "distanceMi": round(float(value.get("distance") or 0.0), 2),
      "minutes": int(value.get("minutes") or 0),
    }
  data = data if isinstance(data, dict) else {}
  return {"all": part(data.get("all")), "week": part(data.get("week"))}


def cached_totals():
  """sunnypilot's own cached copy of comma's totals, and when it was saved."""
  path = os.path.join(PARAMS_DIR, "ApiCache_DriveStats")
  data = read_json(path)
  if not isinstance(data, dict) or not isinstance(data.get("all"), dict):
    return None
  try:
    saved_ms = int(os.path.getmtime(path) * 1000)
  except OSError:
    return None
  return normalize_stats(data), saved_ms


def fetch_totals_from_comma() -> dict:
  dongle_id = read_param("DongleId")
  if not dongle_id or dongle_id == "UnregisteredDevice":
    raise RuntimeError("unregistered")
  from openpilot.common.api import Api, api_get
  token = Api(dongle_id).get_token()
  response = api_get(f"v1.1/devices/{dongle_id}/stats", timeout=15, access_token=token)
  if response.status_code != 200:
    raise RuntimeError(f"http {response.status_code}")
  return normalize_stats(response.json())


# sunnypilot refreshes its copy every 30 s while its UI is up; an hour-old copy still beats a request.
CACHE_FRESH_MS = 60 * 60 * 1000


def refresh_totals(now_ms: int, fetch=fetch_totals_from_comma, cache=cached_totals) -> dict:
  saved = read_json(STATS_FILE)
  saved = saved if isinstance(saved, dict) else {"version": 1}
  cached = cache()
  if cached is not None and now_ms - cached[1] <= CACHE_FRESH_MS:
    saved = {"version": 1, "source": "sunnypilot", "fetchedAtMs": cached[1], **cached[0]}
  else:
    try:
      saved = {"version": 1, "source": "comma", "fetchedAtMs": now_ms, **fetch()}
    except Exception as e:  # offline, unregistered, or comma said no: keep the last totals
      if cached is not None and cached[1] > int(saved.get("fetchedAtMs") or 0):
        saved = {"version": 1, "source": "sunnypilot", "fetchedAtMs": cached[1], **cached[0]}
      saved["lastError"] = str(e)[:120]
      saved["lastErrorAtMs"] = now_ms
      log(f"comma's totals unavailable: {saved['lastError']}")
  write_json_atomic(STATS_FILE, saved)
  return saved


# ---------------------------------------------------------------- Entry

def main(argv=None) -> int:
  os.umask(0o077)  # drives and positions are this comma owner's alone
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  sub = parser.add_subparsers(dest="mode", required=True)
  drive = sub.add_parser("after-drive")
  drive.add_argument("--route", default="")
  drive.add_argument("--start-ms", type=int, default=0)
  drive.add_argument("--end-ms", type=int, default=0)
  drive.add_argument("--duration-s", type=int, required=True)
  sub.add_parser("totals")
  pos = sub.add_parser("position")
  pos.add_argument("--route", required=True)
  args = parser.parse_args(argv)

  now_ms = int(time.time() * 1000)
  if args.mode == "after-drive":
    after_drive(args.route, args.start_ms, args.end_ms, args.duration_s, now_ms)
    refresh_totals(now_ms)
  elif args.mode == "totals":
    refresh_totals(now_ms)
  elif args.mode == "position":
    position(args.route)
  return 0


if __name__ == "__main__":
  sys.exit(main())
