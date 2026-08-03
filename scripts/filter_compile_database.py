#!/usr/bin/env python3
"""Filter compile_commands.json to an exact NUL-delimited source-path set."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from first_party_compile_units import entry_file


def entry_path(entry: dict[str, Any]) -> Path | None:
    filename = entry_file(entry)
    if not filename:
        return None
    path = Path(filename)
    if not path.is_absolute():
        path = Path(str(entry.get("directory", "."))) / path
    return path.resolve()


def filter_database(
    database: Iterable[dict[str, Any]], root: Path, paths: list[Path]
) -> list[dict[str, Any]]:
    root = root.resolve()
    requested = {(root / path).resolve() for path in paths}
    selected: list[dict[str, Any]] = []
    matched: set[Path] = set()
    for entry in database:
        path = entry_path(entry)
        if path is not None and path in requested:
            selected.append(entry)
            matched.add(path)

    missing = sorted(requested - matched)
    if missing:
        joined = ", ".join(str(path) for path in missing)
        raise ValueError(f"missing compile command for changed source: {joined}")
    return selected


def read_nul_paths(path: Path) -> list[Path]:
    return [
        Path(value.decode())
        for value in path.read_bytes().split(b"\0")
        if value
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("paths", type=Path, help="NUL-delimited repository-relative paths")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    try:
        database = json.loads(args.database.read_text())
        if not isinstance(database, list):
            raise ValueError("compile_commands.json must contain a JSON array")
        selected = filter_database(database, args.root, read_nul_paths(args.paths))
        args.output.write_text(json.dumps(selected))
    except (OSError, UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"compile database: selected {len(selected)} exact changed source TU(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
