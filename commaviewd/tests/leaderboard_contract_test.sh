#!/usr/bin/env bash
# The comma's side of the leaderboard (docs/plans/leaderboard.md in RhynoTech/commaview-web): two
# paired-only endpoints that sign on request and nothing else, a private key that stays in its 0600
# file, and support bundles that can't carry it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTROL="$ROOT/commaviewd/src/control_mode.cpp"
SUPPORT="$ROOT/commaviewd/src/support_bundle.cpp"
SCRIPT="$ROOT/comma/src/commaview_drive_stats.py"
RUNNER="$ROOT/commaviewd/scripts/run-unit-tests.sh"

fail() {
  echo "leaderboard contract: $*" >&2
  exit 1
}

require_literal() {
  grep -Fq -- "$1" "$2" || fail "missing in ${2##*/}: $1"
}

# Both endpoints need the pairing token, and a paired runtime (as /commaview/drive-stats does).
require_literal '"/commaview/leaderboard/register"' "$CONTROL"
require_literal '"/commaview/leaderboard/statement"' "$CONTROL"
block="$(sed -n '/req.path == "\/commaview\/leaderboard\/register" || req.path == "\/commaview\/leaderboard\/statement"/,/^  }/p' "$CONTROL")"
grep -Fq 'api_token.empty() || !is_authorized(req, api_token)' <<<"$block" || fail "the endpoints must require a paired token"
grep -Fq 'make_json(401, kUnauthorizedJson)' <<<"$block" || fail "the endpoints must answer 401 without the token"

# The contract's errors.
require_literal '{\"ok\":false,\"error\":\"challenge required\"}' "$CONTROL"
require_literal '{\"ok\":false,\"error\":\"no key\"}' "$CONTROL"
require_literal '{\"ok\":false,\"error\":\"crypto unavailable\"}' "$CONTROL"
require_literal 'make_json(400, kLeaderboardChallengeRequiredJson)' "$CONTROL"
require_literal 'make_json(409, kLeaderboardNoKeyJson)' "$CONTROL"
require_literal 'make_json(503, kLeaderboardNoCryptoJson)' "$CONTROL"
for code in 'EXIT_BAD_REQUEST = 3' 'EXIT_NO_KEY = 4' 'EXIT_NO_CRYPTO = 5'; do
  require_literal "$code" "$SCRIPT"
done
require_literal 'kLeaderboardExitBadRequest = 3' "$CONTROL"
require_literal 'kLeaderboardExitNoKey = 4' "$CONTROL"
require_literal 'kLeaderboardExitNoCrypto = 5' "$CONTROL"

# Signing happens only on request: the leaderboard modes are named once each, in the request
# handlers, and never queued by the drive watcher or any timer.
[[ "$(grep -c '"leaderboard-register"' "$CONTROL")" == 1 ]] || fail "leaderboard-register must be run from one place"
[[ "$(grep -c '"leaderboard-statement"' "$CONTROL")" == 1 ]] || fail "leaderboard-statement must be run from one place"
if grep -E 'queue_drive_script_locked\(.*leaderboard|spawn_drive_script\(.*leaderboard' "$CONTROL"; then
  fail "leaderboard signing must never run in the background"
fi
[[ "$(grep -c 'leaderboard_register_response(' "$CONTROL")" == 2 ]] || fail "register: defined once, called by handle_post only"
[[ "$(grep -c 'leaderboard_statement_response(' "$CONTROL")" == 2 ]] || fail "statement: defined once, called by handle_post only"
if grep -nE '"leaderboard-(register|statement)"' "$SCRIPT" | grep -vE 'add_parser|args.mode ==' >/dev/null; then
  fail "the script signs only for its leaderboard modes"
fi

# The key: its 32-byte seed in data/leaderboard-key.json, owner-only, written on disk before use.
require_literal 'os.path.join(ROOT, "data", "leaderboard-key.json")' "$SCRIPT"
require_literal 'os.path.join(ROOT, "data", "leaderboard-seq.json")' "$SCRIPT"
require_literal 'os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600' "$SCRIPT"
require_literal 'os.fsync(fd)' "$SCRIPT"
# Never logged: no log line or print mentions the seed.
if grep -nE '^\s*(log|print)\(.*(seed|SEED)' "$SCRIPT"; then
  fail "the leaderboard key's seed must never be logged"
fi

# Support bundles: no file from data/ and never the key file; the seed is redacted wherever it shows.
files_block="$(sed -n '/^std::vector<SupportLogFileSpec> support_log_files() {/,/^}/p' "$CONTROL")"
[[ -n "$files_block" ]] || fail "support_log_files() not found"
if grep -Eq '"/data/commaview/data|leaderboard-key\.json"' <<<"$files_block"; then
  fail "support bundles must never include data/ or the leaderboard key"
fi
require_literal 'leaderboard_seed_for_redaction()};' "$CONTROL"
require_literal 'add_literal(secrets.leaderboard_seed, "redacted-secret");' "$SUPPORT"
require_literal '"credential", "seed"}' "$SUPPORT"

# The pipeline runs the tests.
require_literal 'leaderboard_api_integration_test.py' "$RUNNER"
require_literal 'leaderboard_contract_test.sh' "$RUNNER"

echo "PASS: leaderboard endpoint contract"
