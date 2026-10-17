#!/usr/bin/env python3
"""Preserve failing source URLs while invoking the qualified ADB closure validator."""

import sys
import urllib.error

import prefetch_adb_helper_sources as prefetch


def main() -> int:
    try:
        return prefetch.main()
    except urllib.error.HTTPError as error:
        message = f"ADB source fetch failed ({error.url}): HTTP {error.code}"
        print(message, file=sys.stderr)
        raise RuntimeError(message) from error


if __name__ == "__main__":
    raise SystemExit(main())
