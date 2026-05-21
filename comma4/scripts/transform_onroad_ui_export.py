#!/usr/bin/env python3
"""Strict source transformer for CommaView onroad UI export hooks."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def line_indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def find_single(lines: list[str], predicate, description: str) -> int:
    matches = [idx for idx, line in enumerate(lines) if predicate(line)]
    if len(matches) != 1:
        fail(f"expected exactly one {description}, found {len(matches)}")
    return matches[0]


def block_end(lines: list[str], start: int, indent: int) -> int:
    for idx in range(start + 1, len(lines)):
        stripped = lines[idx].strip()
        if not stripped:
            continue
        if line_indent(lines[idx]) <= indent:
            return idx
    return len(lines)


def ensure_export_import(lines: list[str]) -> bool:
    if any(line.strip() == EXPORT_IMPORT for line in lines):
        return False

    insert_at = None
    for idx, line in enumerate(lines):
        if line.startswith("from openpilot.selfdrive.ui."):
            insert_at = idx
            break

    if insert_at is None:
        for idx, line in enumerate(lines):
            if line.startswith("from openpilot.common."):
                insert_at = idx + 1

    if insert_at is None:
        fail("unable to place commaview exporter import")

    lines.insert(insert_at, EXPORT_IMPORT)
    return True


def transform_ui_state(ui_state_path: Path) -> bool:
    if not ui_state_path.is_file():
        fail(f"missing ui_state.py at {ui_state_path}")

    original = ui_state_path.read_text()
    lines = original.splitlines()
    changed = ensure_export_import(lines)

    class_idx = find_single(
        lines,
        lambda line: line.startswith("class UIState") and line.rstrip().endswith(":"),
        "UIState class",
    )
    class_indent = line_indent(lines[class_idx])
    class_stop = block_end(lines, class_idx, class_indent)

    update_matches = [
        idx
        for idx in range(class_idx + 1, class_stop)
        if line_indent(lines[idx]) > class_indent and lines[idx].lstrip().startswith("def update(self")
    ]
    if len(update_matches) != 1:
        fail(f"expected exactly one UIState.update method, found {len(update_matches)}")

    update_idx = update_matches[0]
    update_indent = line_indent(lines[update_idx])
    update_stop = block_end(lines, update_idx, update_indent)
    update_body = lines[update_idx:update_stop]

    device_matches = [
        update_idx + offset
        for offset, line in enumerate(update_body)
        if line.strip() == "device.update()"
    ]
    if len(device_matches) != 1:
        fail(f"expected exactly one device.update() in UIState.update, found {len(device_matches)}")

    if any(EXPORT_PUBLISH in line for line in update_body):
        new_text = "\n".join(lines) + ("\n" if original.endswith("\n") else "")
        if new_text != original:
            ui_state_path.write_text(new_text)
            return True
        return changed

    device_idx = device_matches[0]
    body_indent = lines[device_idx][: line_indent(lines[device_idx])]
    inner_indent = body_indent + "  "
    export_block = [
        f'{body_indent}if not hasattr(self, "_commaview_exporter"):',
        f"{inner_indent}{EXPORT_INSTALL}",
        f"{body_indent}try:",
        f"{inner_indent}{EXPORT_PUBLISH}",
        f"{body_indent}except Exception:",
        f'{inner_indent}cloudlog.exception("commaview ui export publish failed")',
    ]
    lines[device_idx + 1 : device_idx + 1] = export_block
    changed = True

    new_text = "\n".join(lines) + ("\n" if original.endswith("\n") else "")
    if new_text != original:
        ui_state_path.write_text(new_text)
    return changed


def transform(op_root: Path, flavor: str) -> None:
    if flavor not in {"openpilot", "sunnypilot"}:
        fail(f"unsupported flavor: {flavor}")
    transform_ui_state(op_root / "selfdrive" / "ui" / "ui_state.py")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--op-root", required=True, type=Path)
    parser.add_argument("--flavor", required=True, choices=("openpilot", "sunnypilot"))
    args = parser.parse_args(argv)
    transform(args.op_root, args.flavor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
