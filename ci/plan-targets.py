#!/usr/bin/env python3
"""Resolves commaviewd-ci's targets (ci/targets.json) to upstream commits and plans their checks.

Prints a JSON list: each target with `sha` (the upstream commit) and `run_verification`. The
verification build reads only the upstream source, never the UI platform, so targets on the
same commit (release-mici and release-tizi are often one commit) build it once: the first runs
it, the others check only that their UI export patch applies.

  ci/plan-targets.py [name...]   # default: every target
"""
import json
import pathlib
import subprocess
import sys

CI_DIR = pathlib.Path(__file__).resolve().parent


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


def main(names):
    targets = json.loads((CI_DIR / "targets.json").read_text())
    known = {t["name"] for t in targets}
    unknown = [n for n in names if n not in known]
    if unknown:
        sys.exit(f"unknown target(s): {', '.join(unknown)} (known: {', '.join(sorted(known))})")
    if names:
        targets = [t for t in targets if t["name"] in names]
    built_by = {}
    for t in targets:
        t["sha"] = resolve(t["upstream_repo"], t["upstream_ref"])
        if t.get("run_verification", True) is False:
            continue
        if t["sha"] in built_by:
            t["run_verification"] = False
            t["why"] = f"same upstream commit as {built_by[t['sha']]}, which runs the verification build"
        else:
            t["run_verification"] = True
            built_by[t["sha"]] = t["name"]
    print(json.dumps(targets))


if __name__ == "__main__":
    main(sys.argv[1:])
