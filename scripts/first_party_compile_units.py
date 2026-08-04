#!/usr/bin/env python3
"""List configured first-party translation units from compile_commands.json."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx"}


def entry_file(entry: dict[str, Any]) -> str:
    if "file" in entry:
        return str(entry["file"])
    files = entry.get("files") or []
    return str(files[0]) if files else ""


def first_party_compile_units(
    database: Iterable[dict[str, Any]], source_root: Path
) -> list[Path]:
    source_root = source_root.resolve()
    units: set[Path] = set()
    for entry in database:
        filename = entry_file(entry)
        if not filename:
            continue
        path = Path(filename)
        if not path.is_absolute():
            path = Path(str(entry.get("directory", "."))) / path
        path = path.resolve()
        try:
            path.relative_to(source_root)
        except ValueError:
            continue
        if path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        units.add(path)
    return sorted(units)


def load_first_party_compile_units(database_path: Path, source_root: Path) -> list[Path]:
    database = json.loads(database_path.read_text())
    if not isinstance(database, list):
        raise ValueError("compile_commands.json must contain a JSON array")
    return first_party_compile_units(database, source_root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("source_root", type=Path)
    parser.add_argument(
        "--null",
        action="store_true",
        help="terminate paths with NUL for safe xargs -0 consumption",
    )
    args = parser.parse_args()

    try:
        units = load_first_party_compile_units(args.database, args.source_root)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if not units:
        print("error: compile database contains no first-party translation units", file=sys.stderr)
        return 1
    terminator = "\0" if args.null else "\n"
    for path in units:
        sys.stdout.write(f"{path}{terminator}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
