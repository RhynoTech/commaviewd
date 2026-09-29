#!/usr/bin/env bash
set -euo pipefail

# install.sh --help prints the version it resolved, which shows the release a plain install
# would pick without touching a device. Both lookups point at local fixtures.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_SH="$REPO_ROOT/comma/install.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

unset COMMAVIEWD_RELEASE_TAG COMMAVIEWD_DEFAULT_TAG COMMAVIEWD_INSTALLER_REF COMMAVIEWD_VERSION \
  COMMAVIEWD_RELEASE_REPO COMMAVIEWD_CURRENT_RELEASE_URL COMMAVIEWD_RELEASES_API_URL

cat > "$TMP/current-release.json" <<'JSON'
{"appVersion":"0.0.158-alpha","appTag":"app-v0.0.158-alpha","runtimeVersion":"0.0.55-alpha","runtimeTag":"v0.0.55-alpha","minAppVersion":"0.0.158-alpha","maxAppVersion":"0.0.158-alpha"}
JSON
cat > "$TMP/releases.json" <<'JSON'
[
  {"tag_name": "v0.0.56-alpha", "prerelease": true},
  {"tag_name": "v0.0.55-alpha", "prerelease": true}
]
JSON
printf '<html>Service unavailable</html>\n' > "$TMP/not-json.html"
export COMMAVIEWD_RELEASES_API_URL="file://$TMP/releases.json"

installer_version() {
  bash "$INSTALL_SH" "$@" --help | sed -nE 's/^CommaView installer (.*)$/\1/p'
}

expect_version() {
  local want="$1" got="$2" message="$3"
  [[ "$got" == "$want" ]] || { echo "FAIL: $message (expected $want, got ${got:-nothing})" >&2; exit 1; }
}

grep -Fq 'CURRENT_RELEASE_URL="https://commaview.com/api/current-release"' "$INSTALL_SH" || {
  echo "FAIL: install.sh should default to the CommaView current-release API" >&2
  exit 1
}

expect_version "0.0.55-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/current-release.json" installer_version)" \
  "a plain install should use the runtime paired with the current app release"

expect_version "0.0.56-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/missing.json" installer_version)" \
  "an unreachable current-release lookup should fall back to the newest GitHub release"

expect_version "0.0.56-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/not-json.html" installer_version)" \
  "a current-release response without a runtimeTag should fall back to the newest GitHub release"

expect_version "0.0.50-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/current-release.json" installer_version --tag v0.0.50-alpha)" \
  "--tag should win over the current release"

expect_version "0.0.56-alpha" \
  "$(COMMAVIEWD_RELEASE_REPO=someone/fork installer_version)" \
  "another release repo should skip the CommaView current-release lookup"

echo "PASS: install.sh installs the current app release's runtime when no tag is given"
