#!/usr/bin/env python3
"""One effective ccache budget for compilation, cleanup, and cache publication."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

# Allowances follow the retained PR #79 working-set evidence, not the old 250M
# command (Linux's inherited 2G overrode it). Warm-run acceptance remains open.
BUDGETS = {
    "ubuntu-22.04-deb": "500M",
    "ubuntu-24.04-deb": "500M",
    "ubuntu-26.04-deb": "500M",
    # No Focal size log survived; use the release-family allowance provisionally.
    "ubuntu-20.04-appimage": "500M",
    "ubuntu-22.04-asan-lsan": "250M",
    "ubuntu-22.04-ubsan": "250M",
    "intel-qt6": "500M",
    "arm64-qt6": "500M",
    "intel-qt6-asan-ubsan": "250M",
    "intel-qt6-first-party-tsan": "250M",
}


def environment(label: str) -> dict[str, str]:
    if label not in BUDGETS:
        raise ValueError(f"unreviewed ccache label: {label}")
    return {
        "CCACHE_MAXSIZE": BUDGETS[label],
        "CCACHE_COMPRESS": "true",
        "CCACHE_COMPRESSLEVEL": "6",
        # Immutable Linux images preserve compiler metadata and existing master
        # seeds use mtime. Switching that policy makes restore-only PRs cold on
        # every run. Native macOS runner mtimes can vary, so hash compiler bytes.
        "CCACHE_COMPILERCHECK": "mtime" if label.startswith("ubuntu-") else "content",
    }


def size_bytes(value: str) -> int:
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([kKMGT]?)(i?[bB])?\s*", value)
    if not match:
        raise ValueError(f"invalid ccache size: {value!r}")
    power = " KMGT".index(match[2].upper()) if match[2] else 0
    base = 1024 if (match[3] or "").lower().startswith("i") else 1000
    return int(float(match[1]) * base ** power)


def trim(label: str) -> dict:
    env = {**os.environ, **environment(label)}

    def ccache(*args: str) -> str:
        return subprocess.run(["ccache", *args], env=env, check=True,
                              capture_output=True, text=True).stdout.strip()

    effective = ccache("--get-config", "max_size")
    budget = size_bytes(env["CCACHE_MAXSIZE"])
    if size_bytes(effective) != budget:
        raise ValueError(f"ccache budget override: expected {budget}, got {effective}")
    cache_dir = Path(ccache("--get-config", "cache_dir"))
    if not cache_dir.is_absolute():
        raise ValueError("ccache directory must be absolute")
    ccache("--cleanup")
    config = ccache("--show-config")
    payload = 0
    for directory, _, files in os.walk(cache_dir, followlinks=False):
        for name in files:
            path = Path(directory) / name
            if not path.is_symlink():
                payload += path.stat().st_size
    # ccache accounts in filesystem blocks; its metadata is outside object LRU.
    # Report exact payload bytes and reject large overruns rather than claiming
    # a successful bound from configuration alone.
    if payload > budget + max(1_000_000, budget // 100):
        raise ValueError(f"ccache payload exceeds budget after cleanup: {payload} > {budget}")
    return {"label": label, "budget_bytes": budget,
            "effective_max_size_bytes": size_bytes(effective),
            "payload_bytes": payload, "effective_config": config}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "trim"))
    parser.add_argument("--label", required=True)
    parser.add_argument("--github-env", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            values = environment(args.label)
            if args.github_env:
                with args.github_env.open("a", encoding="utf-8") as output:
                    for name, value in values.items():
                        output.write(f"{name}={value}\n")
            print(json.dumps(values, sort_keys=True))
        else:
            print(json.dumps(trim(args.label), indent=2, sort_keys=True))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ccache policy failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
