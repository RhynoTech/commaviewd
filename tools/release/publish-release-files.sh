#!/usr/bin/env bash
# Publishes a runtime release where commas install it from: commaview.com/commaviewd/<tag>/,
# served from the public bucket commaview-runtime-releases (RhynoTech/commaview-web,
# account/terraform/storage.tf and cloudflare/worker.ts).
#
#   tools/release/publish-release-files.sh <tag> <tarball> <tarball.sha256> [--shim comma4/install.sh]
#
# Uploads <tag>/<tarball>, <tag>/<tarball>.sha256 and the installer's companions, <tag>/comma/...
# (every file install.sh fetches, from this checkout of the tag), and with --shim the installer
# shim at the top (install.sh). A tag's files are created, never replaced: a re-run accepts a file
# already there only when it is the same, and fails on any difference. Needs GOOGLE_ACCESS_TOKEN
# (the release account's, from google-github-actions/auth).
set -euo pipefail

BUCKET="${RUNTIME_RELEASES_BUCKET:-commaview-runtime-releases}"
PUBLIC="${RUNTIME_RELEASES_PUBLIC:-https://storage.googleapis.com/${BUCKET}}"
# The checkout of the tag whose comma/ scripts are published (this one, unless backfilling).
REPO_ROOT="${RELEASE_CHECKOUT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

tag="${1:?tag}"
tarball="${2:?tarball}"
checksum="${3:?tarball.sha256}"
shift 3
shim=""
if [[ "${1:-}" == "--shim" ]]; then
  shim="${2:?--shim needs a file}"
fi
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[a-z]+(\.[0-9]+)?)?$ ]] || { echo "not a release tag: $tag" >&2; exit 2; }
: "${GOOGLE_ACCESS_TOKEN:?GOOGLE_ACCESS_TOKEN is required}"

content_type() {
  case "$1" in
    *.tar.gz) echo application/gzip ;;
    *.sh) echo text/x-shellscript ;;
    *.py) echo text/x-python ;;
    *.json) echo application/json ;;
    *) echo text/plain ;;
  esac
}

urlencode() { python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"; }

# Create <name> from <file>, or accept it when the same bytes are already published.
create() {
  local file="$1" name="$2" status
  status="$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer ${GOOGLE_ACCESS_TOKEN}" \
    -H "Content-Type: $(content_type "$name")" \
    -H "Cache-Control: public, max-age=31536000, immutable" \
    --data-binary "@${file}" \
    "https://storage.googleapis.com/upload/storage/v1/b/${BUCKET}/o?uploadType=media&ifGenerationMatch=0&name=$(urlencode "$name")")"
  case "$status" in
    200) echo "published ${name}" ;;
    412)
      # Already there: fine only when it's this file.
      if [[ "$(curl -fsSL "${PUBLIC}/${name}" | sha256sum | cut -d' ' -f1)" == "$(sha256sum < "$file" | cut -d' ' -f1)" ]]; then
        echo "already published ${name}"
      else
        echo "ERROR: ${name} is already published with different contents; a release never changes" >&2
        exit 1
      fi
      ;;
    *) echo "ERROR: publishing ${name} failed (HTTP ${status})" >&2; exit 1 ;;
  esac
}

# The installer shim is the one file replaced: it follows the newest release.
replace() {
  local file="$1" name="$2" status
  status="$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer ${GOOGLE_ACCESS_TOKEN}" \
    -H "Content-Type: $(content_type "$name")" \
    -H "Cache-Control: public, max-age=300" \
    --data-binary "@${file}" \
    "https://storage.googleapis.com/upload/storage/v1/b/${BUCKET}/o?uploadType=media&name=$(urlencode "$name")")"
  [[ "$status" == 200 ]] || { echo "ERROR: publishing ${name} failed (HTTP ${status})" >&2; exit 1; }
  echo "published ${name}"
}

[[ -f "$tarball" && -f "$checksum" ]] || { echo "missing $tarball or $checksum" >&2; exit 1; }
[[ "$(basename "$tarball")" == "commaview-comma-${tag}.tar.gz" ]] || { echo "unexpected tarball name: $tarball" >&2; exit 1; }
(cd "$(dirname "$tarball")" && sha256sum -c "$(basename "$checksum")")

# The companions first: a release is only installable once all of them are there, and the
# tarball last, so nothing finds a tarball whose installer isn't published yet.
mapfile -t companions < <(cd "$REPO_ROOT" && git ls-files comma | grep -v '^comma/tests/' | grep -v '^comma/patches/')
[[ "${#companions[@]}" -gt 0 ]] || { echo "no companion files found under comma/" >&2; exit 1; }
for rel in "${companions[@]}"; do
  create "$REPO_ROOT/$rel" "${tag}/${rel}"
done
create "$checksum" "${tag}/$(basename "$checksum")"
create "$tarball" "${tag}/$(basename "$tarball")"
if [[ -n "$shim" ]]; then
  replace "$shim" "install.sh"
fi
echo "Published ${tag} to ${PUBLIC}/${tag}/"
