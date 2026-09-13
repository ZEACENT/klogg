#!/usr/bin/env python3
"""Validate the suffix appended to the shared first-party CI image prefix."""

from __future__ import annotations

import argparse
import re


CONTAINER_SUFFIX_PATTERN = re.compile(r"_[a-z0-9.-]+")


def validate_container_suffix(value: str) -> str:
    if CONTAINER_SUFFIX_PATTERN.fullmatch(value) is None:
        raise ValueError(
            "container suffix must start with '_' and contain only lowercase "
            "letters, digits, dots, and hyphens"
        )
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suffix", required=True)
    args = parser.parse_args()
    try:
        validate_container_suffix(args.suffix)
    except ValueError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
