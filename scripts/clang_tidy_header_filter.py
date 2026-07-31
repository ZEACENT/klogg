#!/usr/bin/env python3
"""Build a clang-tidy line filter for added lines in changed headers."""

import json
import re
import sys
from pathlib import Path


HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
HEADER_RE = re.compile(r"\.(?:h|hh|hpp|hxx)$")


def build_filter(patch: str) -> list[dict[str, object]]:
    filters: dict[str, list[list[int]]] = {}
    current_file: str | None = None
    expecting_new_file = False
    in_hunk = False

    for line in patch.splitlines():
        if line.startswith("diff --git "):
            current_file = None
            expecting_new_file = False
            in_hunk = False
            continue
        if not in_hunk and (line.startswith("--- a/") or line == "--- /dev/null"):
            expecting_new_file = True
            continue
        if not in_hunk and expecting_new_file:
            expecting_new_file = False
            if line.startswith("+++ b/"):
                path = line[6:]
                current_file = path if HEADER_RE.search(path) else None
                if current_file is not None:
                    filters.setdefault(current_file, [])
            continue

        if current_file is None:
            continue

        match = HUNK_RE.match(line)
        if match is None:
            continue

        in_hunk = True
        start = int(match.group(1))
        count = int(match.group(2) or "1")
        if count > 0:
            filters[current_file].append([start, start + count - 1])

    return [
        {"name": path, "lines": ranges}
        for path, ranges in sorted(filters.items())
        if ranges
    ]


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <unified-diff>", file=sys.stderr)
        return 2

    patch = Path(sys.argv[1]).read_text(encoding="utf-8")
    result = build_filter(patch)
    json.dump(result, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
