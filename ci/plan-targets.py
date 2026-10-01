#!/usr/bin/env python3
"""Resolves verification targets to upstream commits and plans their checks.

  ci/plan-targets.py [--targets FILE] [--markers PREFIX] [name...]

FILE is a target list (default ci/targets.json: commaviewd-ci's; ci/canary-*.json: the canaries').
Prints the targets as a JSON list, each with:
  sha               the upstream commit
  run_verification  whether this target runs the verification build
  pending           whether it has anything left to check (no job when false)
  patch_marker      the pass marker its patch check saves; verify_marker, its verification build's

The verification build reads only the upstream source, never the UI platform, so targets on the
same commit (release-mici and release-tizi are often one commit) build it once: one runs it, the
others only check their UI export patch applies. Every target checks its patch.

With --markers (CI: .github/scripts/pass-marker.sh key over commaviewd's inputs), a check that
already passed for this content (commaviewd's inputs, the upstream commit, the platform) isn't
run again: a merged pull request, or a canary whose upstream didn't move.
"""
import argparse
import json
import pathlib
import subprocess
import sys

CI_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = CI_DIR.parent


def release_pin():
    for line in (CI_DIR / "upstream-refs.env").read_text().splitlines():
        key, _, value = line.partition("=")
        if key.strip() == "COMMAVIEWD_RELEASE_SUNNYPILOT_REF":
            return value.strip().strip("'\"")
    sys.exit("missing COMMAVIEWD_RELEASE_SUNNYPILOT_REF in ci/upstream-refs.env")


def resolve(repo, ref):
    if ref == "release-pin":
        ref = release_pin()
    url = f"https://github.com/{repo}.git"
    for kind in ("heads", "tags"):
        out = subprocess.run(["git", "ls-remote", url, f"refs/{kind}/{ref}"],
                             check=True, capture_output=True, text=True).stdout.split()
        if out:
            return out[0]
    return ref  # a commit SHA


def passed_markers(keys):
    out = subprocess.run([str(REPO_ROOT / ".github/scripts/pass-marker.sh"), "check", *keys],
                         check=True, capture_output=True, text=True).stdout
    return set(out.split())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--targets", default=str(CI_DIR / "targets.json"))
    parser.add_argument("--markers", metavar="PREFIX")
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()

    targets = json.loads(pathlib.Path(args.targets).read_text())
    known = {t["name"] for t in targets}
    unknown = [n for n in args.names if n not in known]
    if unknown:
        sys.exit(f"unknown target(s): {', '.join(unknown)} (known: {', '.join(sorted(known))})")
    if args.names:
        targets = [t for t in targets if t["name"] in args.names]

    prefix = args.markers or "local"
    for t in targets:
        t["sha"] = resolve(t["upstream_repo"], t["upstream_ref"])
        t["patch_marker"] = f"{prefix}-patch-{t['sha']}-{t['ui_platform']}"
        # An unset run_verification means "verify": only an explicit false opts a target out.
        t["wants_verification"] = t.get("run_verification", True) is not False
        t["run_verification"] = False

    keys = {t["patch_marker"] for t in targets}
    keys |= {f"{prefix}-verify-{t['sha']}" for t in targets if t["wants_verification"]}
    passed = passed_markers(sorted(keys)) if args.markers else set()

    for sha in dict.fromkeys(t["sha"] for t in targets if t["wants_verification"]):
        group = [t for t in targets if t["sha"] == sha and t["wants_verification"]]
        verify_marker = f"{prefix}-verify-{sha}"
        if verify_marker in passed:
            for t in group:
                t.setdefault("why", "already verified for this commaviewd and upstream commit")
            continue
        # Prefer a target that has its patch check to do anyway, so verifying adds no job.
        builder = next((t for t in group if t["patch_marker"] not in passed), group[0])
        builder["run_verification"] = True
        builder["verify_marker"] = verify_marker
        for t in group:
            if t is not builder:
                t.setdefault("why", f"same upstream commit as {builder['name']}, which runs the verification build")

    for t in targets:
        t["pending"] = t["run_verification"] or t["patch_marker"] not in passed
        del t["wants_verification"]
    print(json.dumps(targets))


if __name__ == "__main__":
    main()
