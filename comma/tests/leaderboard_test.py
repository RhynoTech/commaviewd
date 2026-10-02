"""The comma's side of the leaderboard (docs/plans/leaderboard.md in RhynoTech/commaview-web, "The
contract"): its own Ed25519 key, registrations and statements signed byte for byte as the account
service verifies them (the plan's test vector), its drives grouped into UTC days, a counter that only
goes up, and a private key that never leaves its file.
"""

import base64
import importlib.util
import json
import os
import stat
import time
from datetime import datetime, timezone
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "comma" / "src" / "commaview_drive_stats.py"
DAY = 86_400_000

# The plan's test vector.
SEED = bytes(range(1, 33))
SEED_B64 = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA"
X = "ebVWLo_mVPlAeLES6KmLp5AfhTrmlb7X4OORC60ElmQ"
KID = "WWpn_pfHui9YKR4CZtQsDGMu7_Gch2zYChfSvnxgtPk"
DONGLE = "0123456789abcdef"
DEVICE_HASH = "ngxqrv6-OtcYbvZ4Met8VlfnOWc6Kx8uEkKKiAV_ZnQ"
CHALLENGE = "qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqo"
ISSUED_MS = 1_790_899_200_000  # 2026-10-02 00:00 UTC
REGISTRATION = (
  "eyJhbGciOiJFZERTQSIsImtpZCI6IldXcG5fcGZIdWk5WUtSNENadFFzREdNdTdfR2NoMnpZQ2hmU3ZueGd0UGsiLCJ0eXAiOiJjdi1sYi1yZWcrand0In0"
  ".eyJjaGFsbGVuZ2UiOiJxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFvIiwiZGV2aWNlSGFzaCI6Im5neHFydjYtT3RjWWJ2"
  "WjRNZXQ4Vmxmbk9XYzZLeDh1RWtLS2lBVl9ablEiLCJpc3N1ZWRBdE1zIjoxNzkwODk5MjAwMDAwLCJtb2RlbCI6ImNvbW1hIDNYIiwicHVibGljS2V5"
  "IjoiZWJWV0xvX21WUGxBZUxFUzZLbUxwNUFmaFRybWxiN1g0T09SQzYwRWxtUSIsInJ1bnRpbWVWZXJzaW9uIjoidjAuMC42MCIsInYiOjF9"
  ".GYwPk9HZpbwdnJnwsg7Eu3FxnA6a8jRO7dIz_jOPGSCBBiF-iG6RkUz5hLJtI3AUSy5xO3hTSUIyHC28T2ybAw"
)
STATEMENT = (
  "eyJhbGciOiJFZERTQSIsImtpZCI6IldXcG5fcGZIdWk5WUtSNENadFFzREdNdTdfR2NoMnpZQ2hmU3ZueGd0UGsiLCJ0eXAiOiJjdi1sYi1zdGF0cytqd3QifQ"
  ".eyJkYXlzIjpbeyJkYXkiOiIyMDI2LTA5LTMwIiwiZGlzdGFuY2VNIjo1MTIzNCwiZHJpdmVzIjoyLCJkdXJhdGlvblMiOjM2MDB9LHsiZGF5IjoiMjAy"
  "Ni0xMC0wMSIsImRpc3RhbmNlTSI6MTIwMCwiZHJpdmVzIjoxLCJkdXJhdGlvblMiOjMwMH1dLCJpc3N1ZWRBdE1zIjoxNzkwODk5MjAwMDAwLCJydW50"
  "aW1lVmVyc2lvbiI6InYwLjAuNjAiLCJzZXEiOjEsInYiOjF9"
  ".KM2SrZb9GWf135AvN7anqi93AzWrfAceWONv5SFXrRq7hjrjlvR0x5GhfQUcN7X247y5sJ9QppaP2VONmHzZBw"
)


@pytest.fixture
def lb(tmp_path, monkeypatch):
  root, params = tmp_path / "commaview", tmp_path / "params"
  params.mkdir()
  root.mkdir()
  monkeypatch.setenv("COMMAVIEW_DRIVE_ROOT", str(root))
  monkeypatch.setenv("COMMAVIEW_DRIVE_PARAMS_DIR", str(params))
  spec = importlib.util.spec_from_file_location(f"commaview_drive_stats_lb_{time.time_ns()}", SCRIPT)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  module.test_root, module.test_params = root, params
  return module


def set_params(lb, **values):
  for key, value in values.items():
    (lb.test_params / key).write_text(value)


def the_vector_comma(lb):
  """The test vector's comma: its key, dongle id, model and runtime."""
  lb.write_private_json(lb.LEADERBOARD_KEY_FILE, {"version": 1, "seed": SEED_B64, "createdMs": ISSUED_MS})
  set_params(lb, DongleId=DONGLE, HardwareModel="comma 3X", HardwareSerial="c0ffee12")
  (lb.test_root / "VERSION").write_text("v0.0.60\n")


def ms(iso: str) -> int:
  return int(datetime.fromisoformat(iso).replace(tzinfo=timezone.utc).timestamp() * 1000)


def drive(start_iso: str, duration_s: int, distance_m: int, route=None) -> dict:
  start = ms(start_iso)
  d = {"startMs": start, "endMs": start + duration_s * 1000, "durationS": duration_s, "distanceM": distance_m}
  if route:
    d["route"] = route
  return d


def write_drives(lb, drives):
  lb.write_json_atomic(lb.DRIVES_FILE, {"version": 1, "sinceMs": drives[0]["startMs"] if drives else None, "drives": drives})


def parts(token: str):
  def decode(part):
    return base64.urlsafe_b64decode(part + "=" * (-len(part) % 4))
  header, payload, signature = token.split(".")
  return json.loads(decode(header)), json.loads(decode(payload)), decode(signature)


def verify(token: str, x: str) -> bool:
  from cryptography.exceptions import InvalidSignature
  from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
  signing_input, signature = token.rsplit(".", 1)
  key = Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(x + "="))
  try:
    key.verify(base64.urlsafe_b64decode(signature + "=="), signing_input.encode("ascii"))
    return True
  except InvalidSignature:
    return False


# ---------------------------------------------------------------- The test vector

def test_the_key_id_and_device_hash_are_the_test_vectors(lb):
  assert lb.b64u(SEED) == SEED_B64 and lb.b64u_32(SEED_B64) == SEED
  key = lb.private_key(SEED)
  assert lb.public_x(key) == X
  assert lb.key_id(X) == KID
  assert lb.device_hash(DONGLE) == DEVICE_HASH
  assert lb.b64u(b"\xaa" * 32) == CHALLENGE


def test_a_registration_is_the_test_vector_byte_for_byte(lb):
  the_vector_comma(lb)
  answer = lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)
  assert answer == {"ok": True, "registration": REGISTRATION, "keyId": KID}


def test_a_statement_is_the_test_vector_byte_for_byte(lb):
  the_vector_comma(lb)
  write_drives(lb, [
    drive("2026-09-30T07:15:00", 2000, 30000, "a"),
    drive("2026-09-30T18:40:00", 1600, 21234, "b"),
    drive("2026-10-01T23:50:00", 300, 1200, "c"),  # ends after midnight: counts on the day it started
  ])
  answer = lb.leaderboard_statement(ISSUED_MS)
  assert answer == {"ok": True, "statement": STATEMENT, "keyId": KID, "seq": 1}


# ---------------------------------------------------------------- The key

def test_the_first_registration_makes_a_private_key_and_later_ones_keep_it(lb):
  set_params(lb, DongleId=DONGLE)
  key_file = Path(lb.LEADERBOARD_KEY_FILE)
  assert not key_file.exists()
  first = lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)
  assert stat.S_IMODE(key_file.stat().st_mode) == 0o600
  saved = json.loads(key_file.read_text())
  assert set(saved) == {"version", "seed", "createdMs"} and saved["version"] == 1 and saved["createdMs"] == ISSUED_MS
  assert lb.b64u_32(saved["seed"]) is not None
  header, payload, _ = parts(first["registration"])
  assert header == {"alg": "EdDSA", "kid": first["keyId"], "typ": "cv-lb-reg+jwt"}
  assert payload["publicKey"] != X and lb.key_id(payload["publicKey"]) == first["keyId"]
  assert verify(first["registration"], payload["publicKey"])
  # A new key's counter starts at 0.
  assert json.loads(Path(lb.LEADERBOARD_SEQ_FILE).read_text()) == {"version": 1, "keyId": first["keyId"], "seq": 0}

  again = lb.leaderboard_register(lb.b64u(b"\x01" * 32), False, ISSUED_MS + 1000)
  assert again["keyId"] == first["keyId"] and json.loads(key_file.read_text()) == saved


def test_rotate_makes_a_new_key_whose_counter_starts_again(lb):
  the_vector_comma(lb)
  write_drives(lb, [drive("2026-10-01T08:00:00", 600, 9000)])
  assert lb.leaderboard_statement(ISSUED_MS)["seq"] == 1
  assert lb.leaderboard_statement(ISSUED_MS)["seq"] == 2
  rotated = lb.leaderboard_register(CHALLENGE, True, ISSUED_MS)
  assert rotated["keyId"] != KID
  assert stat.S_IMODE(Path(lb.LEADERBOARD_KEY_FILE).stat().st_mode) == 0o600
  statement = lb.leaderboard_statement(ISSUED_MS)
  assert statement["keyId"] == rotated["keyId"] and statement["seq"] == 1
  assert verify(statement["statement"], parts(rotated["registration"])[1]["publicKey"])


def test_a_key_file_left_too_open_is_made_private_again(lb):
  the_vector_comma(lb)
  os.chmod(lb.LEADERBOARD_KEY_FILE, 0o644)
  lb.leaderboard_statement(ISSUED_MS)
  assert stat.S_IMODE(Path(lb.LEADERBOARD_KEY_FILE).stat().st_mode) == 0o600


@pytest.mark.parametrize("saved", ["", "[]", '{"version":2,"seed":"%s"}' % SEED_B64, '{"version":1,"seed":"short"}',
                                   '{"version":1,"seed":"%s="}' % SEED_B64])
def test_an_unusable_key_file_is_no_key(lb, saved):
  Path(lb.LEADERBOARD_KEY_FILE).parent.mkdir(parents=True, exist_ok=True)
  Path(lb.LEADERBOARD_KEY_FILE).write_text(saved)
  with pytest.raises(lb.LeaderboardError) as error:
    lb.leaderboard_statement(ISSUED_MS)
  assert (error.value.code, error.value.error) == (lb.EXIT_NO_KEY, "no key")


def test_the_device_hash_falls_back_to_the_serial_and_the_dongle_id_never_leaves(lb):
  set_params(lb, DongleId=DONGLE, HardwareSerial="c0ffee12")
  registration = lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)["registration"]
  assert parts(registration)[1]["deviceHash"] == DEVICE_HASH
  signed = json.dumps(parts(registration)[:2])
  assert DONGLE not in signed and "c0ffee12" not in signed
  for unregistered in ("", "UnregisteredDevice"):
    set_params(lb, DongleId=unregistered)
    payload = parts(lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)["registration"])[1]
    assert payload["deviceHash"] == lb.device_hash("serial:c0ffee12")
    assert "c0ffee12" not in json.dumps(payload)


def test_model_and_runtime_are_optional_and_short(lb):
  set_params(lb, DongleId=DONGLE)
  payload = parts(lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)["registration"])[1]
  assert "model" not in payload and "runtimeVersion" not in payload
  assert set(payload) == {"v", "challenge", "publicKey", "deviceHash", "issuedAtMs"}
  set_params(lb, HardwareModel="m" * 60)
  (lb.test_root / "VERSION").write_text("v" * 60 + "\n")
  payload = parts(lb.leaderboard_register(CHALLENGE, False, ISSUED_MS)["registration"])[1]
  assert payload["model"] == "m" * 40 and payload["runtimeVersion"] == "v" * 40


# ---------------------------------------------------------------- Requests the comma refuses

@pytest.mark.parametrize("challenge", [
  "", "qqq", CHALLENGE + "q", CHALLENGE[:-1] + "r",  # 'r' isn't how 32 bytes end: not canonical
  CHALLENGE[:-1] + "=", CHALLENGE[:-1] + "+", CHALLENGE[:-1] + "/", " " + CHALLENGE[1:],
])
def test_a_bad_challenge_is_refused_before_a_key_is_made(lb, challenge):
  with pytest.raises(lb.LeaderboardError) as error:
    lb.leaderboard_register(challenge, False, ISSUED_MS)
  assert (error.value.code, error.value.error) == (lb.EXIT_BAD_REQUEST, "challenge required")
  assert not Path(lb.LEADERBOARD_KEY_FILE).exists()


def test_no_statement_before_the_first_registration(lb):
  with pytest.raises(lb.LeaderboardError) as error:
    lb.leaderboard_statement(ISSUED_MS)
  assert (error.value.code, error.value.error) == (lb.EXIT_NO_KEY, "no key")
  assert not Path(lb.LEADERBOARD_SEQ_FILE).exists()


def test_without_cryptography_the_comma_says_so(lb, monkeypatch):
  the_vector_comma(lb)
  monkeypatch.setattr(lb, "ed25519", lambda: None)
  for run in (lambda: lb.leaderboard_register(CHALLENGE, False, ISSUED_MS), lambda: lb.leaderboard_statement(ISSUED_MS)):
    with pytest.raises(lb.LeaderboardError) as error:
      run()
    assert (error.value.code, error.value.error) == (lb.EXIT_NO_CRYPTO, "crypto unavailable")
  assert json.loads(Path(lb.LEADERBOARD_KEY_FILE).read_text())["seed"] == SEED_B64  # kept for when it's back


# ---------------------------------------------------------------- The counter

def test_seq_only_goes_up_and_survives_a_restart(lb, tmp_path):
  the_vector_comma(lb)
  assert [lb.leaderboard_statement(ISSUED_MS)["seq"] for _ in range(3)] == [1, 2, 3]
  # A new run of the script (as the control service starts one per request) carries on.
  spec = importlib.util.spec_from_file_location(f"commaview_drive_stats_lb_again_{time.time_ns()}", SCRIPT)
  again = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(again)
  answer = again.leaderboard_statement(ISSUED_MS)
  assert answer["seq"] == 4 and parts(answer["statement"])[1]["seq"] == 4


def test_seq_is_saved_before_the_statement_is_signed(lb, monkeypatch):
  the_vector_comma(lb)
  real_jws = lb.jws

  def fail(*_):
    raise RuntimeError("signing failed")
  monkeypatch.setattr(lb, "jws", fail)
  with pytest.raises(RuntimeError):
    lb.leaderboard_statement(ISSUED_MS)
  monkeypatch.setattr(lb, "jws", real_jws)
  # seq 1 may have gone out: it is never signed again.
  assert lb.leaderboard_statement(ISSUED_MS)["seq"] == 2


def test_a_counter_from_another_key_starts_at_zero(lb):
  the_vector_comma(lb)
  lb.write_private_json(lb.LEADERBOARD_SEQ_FILE, {"version": 1, "keyId": "someone-else", "seq": 41})
  assert lb.leaderboard_statement(ISSUED_MS)["seq"] == 1
  assert stat.S_IMODE(Path(lb.LEADERBOARD_SEQ_FILE).stat().st_mode) == 0o600


# ---------------------------------------------------------------- Days

def test_drives_are_grouped_into_utc_days_by_when_they_started(lb):
  drives = [
    drive("2026-09-28T23:59:59", 120, 2000),   # past midnight: still the 28th
    drive("2026-09-28T10:00:00", 600, 8000),
    drive("2026-09-30T00:00:00", 60, 500),
    {"startMs": 0, "endMs": 60_000, "durationS": 60, "distanceM": 100},  # clock not set: never placed
    {"startMs": ms("2026-09-30T05:00:00"), "endMs": ms("2026-09-30T05:10:00"), "durationS": -5, "distanceM": 12.6},
  ]
  assert lb.drive_days(drives, ISSUED_MS) == [
    {"day": "2026-09-28", "distanceM": 10000, "durationS": 720, "drives": 2},
    {"day": "2026-09-30", "distanceM": 513, "durationS": 60, "drives": 2},
  ]
  assert lb.drive_days([], ISSUED_MS) == []


def test_at_most_the_newest_62_days_and_none_after_the_day_it_is_signed(lb):
  drives = [{"startMs": ISSUED_MS - i * DAY, "endMs": ISSUED_MS - i * DAY + 60_000, "durationS": 60, "distanceM": i}
            for i in range(0, 70)]
  drives.append({"startMs": ISSUED_MS + DAY, "endMs": ISSUED_MS + DAY + 60_000, "durationS": 60, "distanceM": 7})
  days = lb.drive_days(drives, ISSUED_MS)
  assert len(days) == 62
  assert days[0]["day"] == "2026-08-02" and days[-1]["day"] == "2026-10-02"
  assert [d["day"] for d in days] == sorted(d["day"] for d in days)


def test_a_statement_reads_the_drive_list_and_never_changes_it(lb):
  the_vector_comma(lb)
  drives = [drive("2026-09-30T07:15:00", 2000, 30000, "0000002a--8f1e2d3c4b")]
  write_drives(lb, drives)
  before = Path(lb.DRIVES_FILE).read_bytes()
  _, payload, _ = parts(lb.leaderboard_statement(ISSUED_MS)["statement"])
  assert payload["days"] == [{"day": "2026-09-30", "distanceM": 30000, "durationS": 2000, "drives": 1}]
  assert Path(lb.DRIVES_FILE).read_bytes() == before
  # Days only: no routes, no start or end times.
  assert "route" not in json.dumps(payload) and "8f1e2d3c4b" not in json.dumps(payload)
  assert str(drives[0]["startMs"]) not in json.dumps(payload)


# ---------------------------------------------------------------- What the control service sees

def run_main(lb, capsys, *argv):
  code = lb.main(list(argv))
  out, err = capsys.readouterr()
  return code, out, err


def test_the_answer_is_one_json_line_and_never_holds_the_key(lb, capsys):
  set_params(lb, DongleId=DONGLE)
  code, out, err = run_main(lb, capsys, "leaderboard-statement")
  assert (code, json.loads(out)) == (lb.EXIT_NO_KEY, {"ok": False, "error": "no key"})
  code, out, err = run_main(lb, capsys, "leaderboard-register", "--challenge=" + CHALLENGE)
  assert code == 0 and out.count("\n") == 1
  answer = json.loads(out)
  assert set(answer) == {"ok", "registration", "keyId"} and answer["ok"] is True
  seed = json.loads(Path(lb.LEADERBOARD_KEY_FILE).read_text())["seed"]
  code, out2, err2 = run_main(lb, capsys, "leaderboard-statement")
  assert code == 0 and json.loads(out2)["seq"] == 1 and set(json.loads(out2)) == {"ok", "statement", "keyId", "seq"}
  for text in (out, err, out2, err2):
    assert seed not in text and base64.urlsafe_b64decode(seed + "=").hex() not in text


def test_a_challenge_may_start_with_a_dash(lb, capsys):
  dashed = lb.b64u(b"\xf8" + bytes(31))
  assert dashed.startswith("-")
  code, out, _ = run_main(lb, capsys, "leaderboard-register", "--challenge=" + dashed)
  assert code == 0 and parts(json.loads(out)["registration"])[1]["challenge"] == dashed


def test_failures_answer_without_details(lb, capsys, monkeypatch):
  code, out, _ = run_main(lb, capsys, "leaderboard-register", "--challenge=nope")
  assert (code, json.loads(out)) == (lb.EXIT_BAD_REQUEST, {"ok": False, "error": "challenge required"})
  monkeypatch.setattr(lb, "ed25519", lambda: None)
  code, out, _ = run_main(lb, capsys, "leaderboard-register", "--challenge=" + CHALLENGE)
  assert (code, json.loads(out)) == (lb.EXIT_NO_CRYPTO, {"ok": False, "error": "crypto unavailable"})
  monkeypatch.undo()
  the_vector_comma(lb)

  def broken(*_):
    raise ValueError(SEED_B64)  # whatever an unexpected error says stays out of the answer and the log
  monkeypatch.setattr(lb, "drive_days", broken)
  code, out, err = run_main(lb, capsys, "leaderboard-statement")
  assert (code, json.loads(out)) == (1, {"ok": False, "error": "leaderboard failed"})
  assert SEED_B64 not in out and SEED_B64 not in err and "ValueError" in err
