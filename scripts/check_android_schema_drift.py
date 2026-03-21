#!/usr/bin/env python3
"""Structured Android schema drift checker for commaviewd."""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Dict, Iterable, List

STRUCT_RE = re.compile(r"\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\b")
ENUM_RE = re.compile(r"\benum\s+([A-Za-z_][A-Za-z0-9_]*)\b")
FIELD_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*@([0-9]+)\s*:\s*([^;]+);")
ENUM_VALUE_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*@([0-9]+);$")
COMMENT_RE = re.compile(r"#.*$")


def _load_json(value):
    if isinstance(value, (str, Path)):
        return json.loads(Path(value).read_text())
    return value


def load_contract(data):
    payload = _load_json(data)
    payload.setdefault("version", 1)
    payload.setdefault("services", {})
    return payload


def load_ignores(data):
    payload = _load_json(data)
    payload.setdefault("version", 1)
    payload.setdefault("ignores", [])
    return payload


def _empty_service(file_path: str):
    return {"file": file_path, "fields": {}, "enums": {}}


def parse_schema_tree(root):
    root = Path(root)
    services: Dict[str, dict] = {}
    for path in sorted(root.rglob("*.capnp")):
        relative = path.relative_to(root).as_posix()
        _parse_capnp_file(path, relative, services)
    return {"services": services}


def _parse_capnp_file(path: Path, relative: str, services: Dict[str, dict]) -> None:
    lines = path.read_text().splitlines()
    stack: List[dict] = []

    for raw_line in lines:
        line = COMMENT_RE.sub("", raw_line).strip()
        if not line:
            continue

        closing = line.count("}")
        while closing > 0 and stack:
            stack.pop()
            closing -= 1

        struct_match = STRUCT_RE.search(line)
        if struct_match:
            name = struct_match.group(1)
            full_name = name
            if stack and stack[-1]["kind"] == "struct":
                full_name = f"{stack[-1]['name']}.{name}"
            services.setdefault(full_name, _empty_service(relative))
            stack.append({"kind": "struct", "name": full_name})
            continue

        enum_match = ENUM_RE.search(line)
        if enum_match and stack and stack[-1]["kind"] == "struct":
            enum_name = enum_match.group(1)
            services[stack[-1]["name"]]["enums"].setdefault(enum_name, {})
            stack.append({"kind": "enum", "name": enum_name, "service": stack[-1]["name"]})
            continue

        if stack and stack[-1]["kind"] == "enum":
            value_match = ENUM_VALUE_RE.match(line)
            if value_match:
                services[stack[-1]["service"]]["enums"][stack[-1]["name"]][value_match.group(1)] = {
                    "ordinal": int(value_match.group(2))
                }
            continue

        if stack and stack[-1]["kind"] == "struct":
            field_match = FIELD_RE.match(line)
            if field_match:
                services[stack[-1]["name"]]["fields"][field_match.group(1)] = {
                    "ordinal": int(field_match.group(2)),
                    "type": field_match.group(3).strip(),
                }


def _is_ignored(ignores: dict, label: str, item: dict) -> bool:
    for rule in ignores.get("ignores", []):
        upstream = rule.get("upstream", "both")
        if upstream not in ("both", label):
            continue
        if rule.get("service") not in (None, item["service"]):
            continue
        if rule.get("symbol") not in (None, item["symbol"]):
            continue
        if rule.get("driftClass") not in (None, item["driftClass"]):
            continue
        return True
    return False


def diff_contract(contract, upstream, ignores, label="upstream"):
    contract = load_contract(contract)
    upstream = parse_schema_tree(upstream) if isinstance(upstream, (str, Path)) else upstream
    ignores = load_ignores(ignores)
    items: List[dict] = []
    contract_services = contract.get("services", {})
    upstream_services = upstream.get("services", {})

    for service_name, expected in contract_services.items():
        actual = upstream_services.get(service_name)
        if actual is None:
            items.append({"service": service_name, "symbol": service_name, "driftClass": "service-removed", "file": expected.get("file")})
            continue

        expected_fields = expected.get("fields", {})
        actual_fields = actual.get("fields", {})
        for field_name, field_info in expected_fields.items():
            if field_name not in actual_fields:
                items.append({"service": service_name, "symbol": field_name, "driftClass": "field-removed", "file": actual.get("file")})
                continue
            actual_info = actual_fields[field_name]
            if actual_info.get("type") != field_info.get("type") or actual_info.get("ordinal") != field_info.get("ordinal"):
                items.append({"service": service_name, "symbol": field_name, "driftClass": "field-type-changed", "file": actual.get("file")})
        for field_name in actual_fields:
            if field_name not in expected_fields:
                items.append({"service": service_name, "symbol": field_name, "driftClass": "field-added", "file": actual.get("file")})

        expected_enums = expected.get("enums", {})
        actual_enums = actual.get("enums", {})
        for enum_name, enum_values in expected_enums.items():
            actual_values = actual_enums.get(enum_name, {})
            for value_name, value_info in enum_values.items():
                if value_name not in actual_values:
                    items.append({"service": f"{service_name}.{enum_name}", "symbol": value_name, "driftClass": "enum-value-removed", "file": actual.get("file")})
                    continue
                if actual_values[value_name].get("ordinal") != value_info.get("ordinal"):
                    items.append({"service": f"{service_name}.{enum_name}", "symbol": value_name, "driftClass": "enum-value-changed", "file": actual.get("file")})
            for value_name in actual_values:
                if value_name not in enum_values:
                    items.append({"service": f"{service_name}.{enum_name}", "symbol": value_name, "driftClass": "enum-value-added", "file": actual.get("file")})

    for service_name, actual in upstream_services.items():
        if service_name not in contract_services:
            items.append({"service": service_name, "symbol": service_name, "driftClass": "service-added", "file": actual.get("file")})

    items = sorted(items, key=lambda item: (item["service"], item["symbol"], item["driftClass"]))
    items = [item for item in items if not _is_ignored(ignores, label, item)]
    return {
        "label": label,
        "items": items,
        "unignoredCount": len(items),
        "servicesChecked": len(contract_services),
    }


def _build_parser():
    parser = argparse.ArgumentParser(description="Structured Android schema drift checker")
    parser.add_argument("--contract", default="android-schema/contract-manifest.json")
    parser.add_argument("--ignore-manifest", default="android-schema/ignore-manifest.json")
    parser.add_argument("--manifest", default="android-schema/manifest.json")
    parser.add_argument("--upstream-root", required=True)
    parser.add_argument("--label", default="upstream")
    parser.add_argument("--mode", choices=["fail", "warn", "report", "suggest"], default="fail")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    contract = load_contract(args.contract)
    ignores = load_ignores(args.ignore_manifest)
    upstream = parse_schema_tree(args.upstream_root)
    report = diff_contract(contract, upstream, ignores, label=args.label)
    report.update(
        {
            "contract": str(args.contract),
            "ignoreManifest": str(args.ignore_manifest),
            "manifest": str(args.manifest),
            "upstreamRoot": str(args.upstream_root),
        }
    )
    Path("dist").mkdir(exist_ok=True)
    out = Path("dist/android-schema-drift.json")
    out.write_text(json.dumps(report, indent=2) + "\n")

    summary = [
        f"### Android schema drift check ({args.label})",
        f"- Contract: `{args.contract}`",
        f"- Ignore manifest: `{args.ignore_manifest}`",
        f"- Upstream root: `{args.upstream_root}`",
        f"- Unignored drift count: `{report['unignoredCount']}`",
    ]
    if report["items"]:
        summary.append("")
        summary.append("**Unignored drift**")
        for item in report["items"]:
            summary.append(f"- `{item['service']}` :: `{item['symbol']}` ({item['driftClass']})")
    text = "\n".join(summary)
    print(text)
    step = os.environ.get("GITHUB_STEP_SUMMARY")
    if step:
        with open(step, "a", encoding="utf-8") as fh:
            fh.write(text + "\n")

    if args.mode == "suggest":
        print("\nSuggested next step: review candidate contract/ignore changes manually.")
        return 0
    if args.mode in ("warn", "report"):
        return 0
    return 1 if report["unignoredCount"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
