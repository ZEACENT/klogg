#!/usr/bin/env python3
"""Run no-display, no-GUI subprocess acceptance against a built kloggctl."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: controlcli_smoke.py /absolute/path/to/kloggctl")
    binary = str(Path(sys.argv[1]).resolve(strict=True))
    environment = dict(os.environ, QT_QPA_PLATFORM="nonexistent-cli-smoke-platform")
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    checks = 0

    def run(arguments, expected, data=b"", structured=True):
        nonlocal checks
        completed = subprocess.run(
            [binary, *arguments], input=data, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment, timeout=10, check=False,
        )
        assert completed.returncode == expected, (
            f"Unexpected CLI exit status: expected {expected}, got {completed.returncode}"
        )
        assert not completed.stderr, "Unexpected CLI diagnostics"
        assert completed.stdout.endswith(b"\n"), "Missing output terminator"
        checks += 1
        if not structured:
            assert b"--params-file" in completed.stdout
            return None
        value = json.loads(completed.stdout)
        assert isinstance(value, dict) and value["jsonrpc"] == "2.0"
        assert ("result" in value) != ("error" in value)
        return value

    run(["--help"], 0, structured=False)
    version = run(["--version"], 0)
    assert version["result"]["version"]
    assert version["result"]["protocol_version"] == 1
    run(["version"], 0)
    run([], 2)
    run(["state", "get", "--unknown"], 2)
    run(["state", "get", "--timeout-ms", "-1"], 2)
    run(["state", "get", "--timeout-ms", "2147483648"], 2)
    run(["call", "label.set", "--params", "[]"], 2)
    malformed = run(["call", "label.set", "--params", "sensitive-cli-payload"], 2)
    assert "sensitive-cli-payload" not in json.dumps(malformed)
    run(["call", "label.set", "--params-file", "-"], 2, b"null")
    run(["call", "label.set", "--params-file", "-"], 2, b" " * (1048576 + 1))

    # Explicitly missing selection cannot send a request to a real GUI, even
    # when another local instance happens to be running during this smoke.
    target = ["--instance", "cli-smoke-deliberately-missing-instance"]
    result = run([*target, "state", "get"], 3)
    assert result["error"]["data"]["code"] in {"INSTANCE_NOT_FOUND", "DISCOVERY_FAILED"}
    params = json.dumps({"line": "18446744073709551615", "text": "café"}, ensure_ascii=False).encode()
    piped_call = [*target, "call", "label.set", "--params-file", "-"]
    run(piped_call, 3, params)
    # Multiple reads, then a short read and a clean EOF. Windows text-mode
    # Ctrl-Z must not hide invalid trailing data or bypass the byte budget.
    run(piped_call, 3, b" " * 17000 + params)
    run(piped_call, 2, params + b"\x1anot-json")
    with tempfile.TemporaryDirectory(prefix="klogg-cli-smoke-") as directory:
        path = Path(directory) / "params café.json"
        path.write_bytes(params)
        run([*target, "call", "label.set", "--params-file", str(path)], 3)
        path.write_bytes(b" " * (1048576 + 1))
        run(["call", "label.set", "--params-file", str(path)], 2)
    print(f"CLI subprocess smoke passed: {checks} checks")


if __name__ == "__main__":
    main()
