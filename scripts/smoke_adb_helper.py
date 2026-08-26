#!/usr/bin/env python3
"""Smoke-test a complete ADB client and a private loopback server instance."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time


PROBES = [
    "version",
    "complete-client",
    "loopback-private-server",
    "smart-socket-host-version",
    "no-lingering-process",
]


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_client(adb: pathlib.Path, command: str, timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(adb), command],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def terminate(process: subprocess.Popen[bytes], timeout: float) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        process.terminate()
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=timeout)


def recv_exact(connection: socket.socket, size: int) -> bytes:
    payload = b""
    while len(payload) < size:
        chunk = connection.recv(size - len(payload))
        if not chunk:
            raise RuntimeError(f"short smart-socket response: expected {size}, got {len(payload)}")
        payload += chunk
    return payload


def smart_socket_version(port: int, deadline: float) -> str:
    last_error: Exception | None = None
    request = b"host:version"
    frame = f"{len(request):04x}".encode("ascii") + request
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25) as connection:
                connection.sendall(frame)
                status = recv_exact(connection, 4)
                if status != b"OKAY":
                    raise RuntimeError(f"host:version returned {status!r}")
                size_text = recv_exact(connection, 4)
                size = int(size_text.decode("ascii"), 16)
                payload = recv_exact(connection, size)
                return payload.decode("ascii")
        except (ConnectionError, OSError, RuntimeError, ValueError) as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"private ADB server did not answer host:version: {last_error}")


def write_report(path: pathlib.Path | None, report: dict) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adb", required=True, type=pathlib.Path)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--timeout-seconds", default=10.0, type=float)
    parser.add_argument("--json-output", type=pathlib.Path)
    args = parser.parse_args()

    report: dict = {
        "schema_version": 1,
        "receipt_kind": "binary-smoke",
        "passed_probes": [],
    }
    server: subprocess.Popen[bytes] | None = None
    isolated_home = tempfile.TemporaryDirectory(prefix="klogg-adb-smoke-")
    try:
        adb = args.adb.resolve(strict=True)
        if adb.is_symlink() or not adb.is_file():
            raise RuntimeError(f"ADB helper is not a regular executable: {adb}")
        if os.name != "nt" and not os.access(adb, os.X_OK):
            raise RuntimeError(f"ADB helper is not executable: {adb}")
        report["helper_path"] = str(adb)
        report["helper_sha256"] = sha256(adb)

        version = run_client(adb, "version", args.timeout_seconds)
        if version.returncode != 0 or "Android Debug Bridge version" not in version.stdout:
            raise RuntimeError("ADB version probe failed or did not identify Android Debug Bridge")
        report["version_output"] = version.stdout.strip()
        report["passed_probes"].append("version")

        help_result = run_client(adb, "help", args.timeout_seconds)
        help_text = help_result.stdout + help_result.stderr
        if help_result.returncode != 0 or not all(token in help_text.lower() for token in ("devices", "shell")):
            raise RuntimeError("complete ADB client help probe failed; server-only fork is not acceptable")
        report["passed_probes"].append("complete-client")

        port = args.port or free_loopback_port()
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        # ADB's portable loopback syntax is tcp:<port>. The implementation binds
        # 127.0.0.1; tcp:<hostname>:<port> is rejected by the native server.
        server_socket_spec = f"tcp:{port}"
        server_environment = {
            key: value for key, value in os.environ.items() if not key.startswith("ADB_")
        }
        server_environment["HOME"] = isolated_home.name
        server_environment["USERPROFILE"] = isolated_home.name
        server = subprocess.Popen(
            [str(adb), "-L", server_socket_spec, "server", "nodaemon"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=os.name != "nt",
            creationflags=creation_flags,
            env=server_environment,
        )
        report["server_pid"] = server.pid
        report["server_endpoint"] = f"tcp:127.0.0.1:{port}"
        report["passed_probes"].append("loopback-private-server")

        report["host_version"] = smart_socket_version(port, time.monotonic() + args.timeout_seconds)
        report["passed_probes"].append("smart-socket-host-version")
    except Exception as error:
        report["error"] = str(error)
        print(f"ADB helper smoke failure: {error}")
        return_code = 1
    else:
        return_code = 0
    finally:
        if server is not None:
            try:
                terminate(server, max(1.0, args.timeout_seconds / 2.0))
            except Exception as error:
                report["cleanup_error"] = str(error)
                return_code = 1
            if not process_exists(server.pid):
                report["passed_probes"].append("no-lingering-process")
            else:
                report["cleanup_error"] = f"ADB server process {server.pid} is still running"
                return_code = 1
        isolated_home.cleanup()
        write_report(args.json_output, report)

    if return_code == 0 and report.get("passed_probes") != PROBES:
        print(f"ADB helper smoke failure: incomplete probe set {report.get('passed_probes')}")
        return 1
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
