#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
BIN="${COMMAVIEWD_HOST_BIN:-$REPO_ROOT/dist/commaviewd-host}"
TOKEN="${COMMAVIEWD_PAIRING_TEST_TOKEN:-commaview-test-token}"
PORT="${COMMAVIEWD_PAIRING_TEST_PORT:-0}"
TMP="$(mktemp -d)"
PID=""

cleanup() {
  if [[ -n "$PID" ]]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

if [[ ! -x "$BIN" ]]; then
  echo "[ERR] Missing host runtime binary: $BIN" >&2
  echo "Run commaviewd/scripts/build-ubuntu.sh first." >&2
  exit 2
fi

if [[ "$PORT" == "0" ]]; then
  PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
fi

COMMAVIEWD_API_TOKEN="$TOKEN" "$BIN" control --port "$PORT" >"$TMP/server.log" 2>&1 &
PID="$!"

for _ in {1..100}; do
  if grep -q "commaviewd control: listening" "$TMP/server.log"; then
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    cat "$TMP/server.log" >&2 || true
    echo "[ERR] commaviewd control exited before listening" >&2
    exit 1
  fi
  sleep 0.1
done

grep -q "commaviewd control: listening" "$TMP/server.log" || {
  cat "$TMP/server.log" >&2 || true
  echo "[ERR] timed out waiting for commaviewd control" >&2
  exit 1
}

BASE="http://127.0.0.1:$PORT"
VERSION="$TMP/version.json"
UNAUTH="$TMP/create-unauthorized.json"
WRONG_AUTH="$TMP/create-wrong-auth.json"
CREATE="$TMP/create.json"
ROTATE="$TMP/create-rotated.json"
OLD_REDEEM="$TMP/old-redeem.json"
MISSING_REDEEM="$TMP/missing-redeem.json"
REDEEM="$TMP/redeem.json"
SECOND="$TMP/second-redeem.json"
BAD_REDEEM="$TMP/bad-redeem.json"

curl -fsS "$BASE/commaview/version" > "$VERSION"
UNAUTH_STATUS="$(curl -sS -o "$UNAUTH" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/create")"
WRONG_AUTH_STATUS="$(curl -sS -o "$WRONG_AUTH" -w '%{http_code}' -X POST -H 'X-CommaView-Token: wrong-token' -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/create")"
CREATE_STATUS="$(curl -sS -o "$CREATE" -w '%{http_code}' -X POST -H "X-CommaView-Token: $TOKEN" -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/create")"
CODE="$(python3 - <<'PY' "$CREATE"
import json, sys
print(json.load(open(sys.argv[1]))["pairCode"])
PY
)"
ROTATE_STATUS="$(curl -sS -o "$ROTATE" -w '%{http_code}' -X POST -H "X-CommaView-Token: $TOKEN" -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/create")"
ROTATED_CODE="$(python3 - <<'PY' "$ROTATE"
import json, sys
print(json.load(open(sys.argv[1]))["pairCode"])
PY
)"
OLD_REDEEM_STATUS="$(curl -sS -o "$OLD_REDEEM" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "{\"pairCode\":\"$CODE\"}" "$BASE/pairing/redeem")"
MISSING_REDEEM_STATUS="$(curl -sS -o "$MISSING_REDEEM" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/redeem")"
REDEEM_STATUS="$(curl -sS -o "$REDEEM" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "{\"pairCode\":\"$ROTATED_CODE\"}" "$BASE/pairing/redeem")"
SECOND_STATUS="$(curl -sS -o "$SECOND" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "{\"pairCode\":\"$ROTATED_CODE\"}" "$BASE/pairing/redeem")"
curl -fsS -X POST -H "X-CommaView-Token: $TOKEN" -H 'Content-Type: application/json' -d '{}' "$BASE/pairing/create" > /dev/null
BAD_STATUS="$(curl -sS -o "$BAD_REDEEM" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{"pairCode":"BAD-CODE"}' "$BASE/pairing/redeem")"

python3 - <<'PY' "$TOKEN" "$VERSION" "$UNAUTH_STATUS" "$UNAUTH" "$WRONG_AUTH_STATUS" "$WRONG_AUTH" "$CREATE_STATUS" "$CREATE" "$ROTATE_STATUS" "$ROTATE" "$OLD_REDEEM_STATUS" "$OLD_REDEEM" "$MISSING_REDEEM_STATUS" "$MISSING_REDEEM" "$REDEEM_STATUS" "$REDEEM" "$SECOND_STATUS" "$SECOND" "$BAD_STATUS" "$BAD_REDEEM"
import json, sys
(
    token, version_path, unauth_status, unauth_path, wrong_auth_status, wrong_auth_path,
    create_status, create_path, rotate_status, rotate_path, old_redeem_status, old_redeem_path,
    missing_redeem_status, missing_redeem_path, redeem_status, redeem_path,
    second_status, second_path, bad_status, bad_path,
) = sys.argv[1:]
version = json.load(open(version_path))
unauth = json.load(open(unauth_path))
wrong_auth = json.load(open(wrong_auth_path))
create = json.load(open(create_path))
rotate = json.load(open(rotate_path))
old_redeem = json.load(open(old_redeem_path))
missing_redeem = json.load(open(missing_redeem_path))
redeem = json.load(open(redeem_path))
second = json.load(open(second_path))
bad = json.load(open(bad_path))
assert version.get("runtimeVersion") is not None, version
assert unauth_status == "401", (unauth_status, unauth)
assert unauth.get("ok") is False and unauth.get("error") == "unauthorized", unauth
assert wrong_auth_status == "401", (wrong_auth_status, wrong_auth)
assert wrong_auth.get("ok") is False and wrong_auth.get("error") == "unauthorized", wrong_auth
assert create_status == "200", (create_status, create)
assert create.get("ok") is True, create
assert create.get("pairCode"), create
assert create.get("expiresInSec") == 300, create
assert create.get("pairCode") in create.get("pairingUri", ""), create
assert rotate_status == "200", (rotate_status, rotate)
assert rotate.get("ok") is True and rotate.get("pairCode") != create.get("pairCode"), rotate
assert old_redeem_status == "400", (old_redeem_status, old_redeem)
assert old_redeem.get("ok") is False and old_redeem.get("error") == "invalid pairing code", old_redeem
assert missing_redeem_status == "400", (missing_redeem_status, missing_redeem)
assert missing_redeem.get("ok") is False and missing_redeem.get("error") == "pairCode required", missing_redeem
assert redeem_status == "200", (redeem_status, redeem)
assert redeem.get("ok") is True and redeem.get("apiToken") == token, redeem
assert second_status == "400", (second_status, second)
assert second.get("ok") is False and "unavailable" in second.get("error", ""), second
assert bad_status == "400", (bad_status, bad)
assert bad.get("ok") is False and bad.get("error") == "invalid pairing code", bad
PY

echo "PASS: control mode pairing integration"
