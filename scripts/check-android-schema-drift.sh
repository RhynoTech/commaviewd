#!/usr/bin/env bash
set -euo pipefail

MANIFEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android-schema/manifest.json"
UPSTREAM_ROOT=""
MODE="fail"
LABEL="upstream"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest)
      MANIFEST="$2"; shift 2 ;;
    --upstream-root)
      UPSTREAM_ROOT="$2"; shift 2 ;;
    --label)
      LABEL="$2"; shift 2 ;;
    --mode)
      MODE="$2"; shift 2 ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2 ;;
  esac
done

if [[ -z "$UPSTREAM_ROOT" ]]; then
  echo "Usage: $0 --upstream-root <path> [--manifest <path>] [--label <label>] [--mode fail|warn]" >&2
  exit 2
fi

[[ -f "$MANIFEST" ]] || { echo "FAIL: missing manifest $MANIFEST" >&2; exit 2; }
[[ -d "$UPSTREAM_ROOT" ]] || { echo "FAIL: missing upstream root $UPSTREAM_ROOT" >&2; exit 2; }
mkdir -p dist

python3 - <<'PY' "$MANIFEST" "$UPSTREAM_ROOT" "$LABEL" "$MODE"
import hashlib, json, pathlib, sys
manifest_path = pathlib.Path(sys.argv[1])
upstream_root = pathlib.Path(sys.argv[2])
label = sys.argv[3]
mode = sys.argv[4]
manifest = json.loads(manifest_path.read_text())
results = []
missing = []
drift = []
for entry in manifest.get('files', []):
    upstream_rel = entry.get('upstreamPath') or entry['path'].replace('schemas/', '')
    target = upstream_root / upstream_rel
    if not target.exists():
        missing.append({'upstreamPath': upstream_rel})
        continue
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    item = {
        'path': entry['path'],
        'upstreamPath': upstream_rel,
        'expectedSha256': entry['sha256'],
        'actualSha256': digest,
        'match': digest == entry['sha256'],
    }
    results.append(item)
    if digest != entry['sha256']:
        drift.append(item)
report = {
    'label': label,
    'manifest': str(manifest_path),
    'upstreamRoot': str(upstream_root),
    'sourceRepo': manifest.get('sourceRepo'),
    'sources': manifest.get('sources', {}),
    'missing': missing,
    'results': results,
    'driftCount': len(drift),
    'missingCount': len(missing),
}
out = pathlib.Path('dist/android-schema-drift.json')
out.write_text(json.dumps(report, indent=2) + '\n')
summary = []
summary.append(f"### Android schema drift check ({label})")
summary.append(f"- Manifest: `{manifest_path}`")
summary.append(f"- Upstream root: `{upstream_root}`")
summary.append(f"- Drift count: `{len(drift)}`")
summary.append(f"- Missing count: `{len(missing)}`")
if drift:
    summary.append('')
    summary.append('**Mismatched files**')
    for item in drift:
        summary.append(f"- `{item['upstreamPath']}`")
if missing:
    summary.append('')
    summary.append('**Missing files**')
    for item in missing:
        summary.append(f"- `{item['upstreamPath']}`")
step = pathlib.Path(pathlib.os.environ['GITHUB_STEP_SUMMARY']) if 'GITHUB_STEP_SUMMARY' in pathlib.os.environ else None
if step:
    with step.open('a') as fh:
        fh.write('\n'.join(summary) + '\n')
print('\n'.join(summary))
if (drift or missing) and mode != "warn":
    sys.exit(1)
PY
