#!/usr/bin/env python3
"""Verify the target-bound SHA-256 envelope for a source-built ADB helper artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import stat


SHA256_LINE = re.compile(r"^([0-9a-f]{64})  ([^\r\n]+)$")
REQUIRED_RECEIPTS = (
    "receipt.json",
    "smoke.json",
    "package-smoke.json",
    "package-verification.json",
)


class EnvelopeError(RuntimeError):
    pass


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value: str) -> pathlib.PurePosixPath:
    if "\\" in value:
        raise EnvelopeError(f"checksum path contains a backslash: {value}")
    relative = pathlib.PurePosixPath(value)
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise EnvelopeError(f"unsafe checksum path: {value}")
    return relative


def verify_checksum_file(envelope: pathlib.Path) -> dict[str, str]:
    if not envelope.is_file() or envelope.is_symlink():
        raise EnvelopeError(f"missing or invalid checksum envelope: {envelope}")
    root = envelope.parent.resolve()
    entries: dict[str, str] = {}
    for line_number, line in enumerate(envelope.read_text(encoding="utf-8").splitlines(), 1):
        match = SHA256_LINE.fullmatch(line)
        if match is None:
            raise EnvelopeError(f"invalid checksum envelope line {line_number}: {line}")
        expected, value = match.groups()
        relative = safe_relative(value)
        normalized = relative.as_posix()
        if normalized in entries:
            raise EnvelopeError(f"duplicate checksum envelope entry: {normalized}")
        path = root.joinpath(*relative.parts)
        if not path.exists() or path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise EnvelopeError(f"missing or invalid checksummed artifact member: {normalized}")
        actual = sha256(path)
        if actual != expected:
            raise EnvelopeError(
                f"checksum mismatch for {normalized}: expected {expected}, got {actual}"
            )
        entries[normalized] = expected
    if not entries:
        raise EnvelopeError("checksum envelope is empty")
    return entries


def read_json(path: pathlib.Path, label: str) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EnvelopeError(f"invalid {label}: {error}") from error
    if not isinstance(document, dict):
        raise EnvelopeError(f"invalid {label}: root must be an object")
    return document


def verify_artifact_envelope(
    lock_path: pathlib.Path, artifact_root: pathlib.Path, expected_target: str
) -> None:
    envelope = artifact_root / "SHA256SUMS"
    entries = verify_checksum_file(envelope)
    lock = read_json(lock_path, "ADB helper lock")
    target_plan = lock.get("targets", {}).get(expected_target)
    if not isinstance(target_plan, dict):
        raise EnvelopeError(f"unknown locked ADB helper target: {expected_target}")

    required = set(REQUIRED_RECEIPTS)
    helper_name = "adb.exe" if expected_target.startswith("windows-") else "adb"
    required.add(f"helpers/{helper_name}")
    runtime_files = target_plan.get("usb", {}).get("runtime_files", [])
    if not isinstance(runtime_files, list):
        raise EnvelopeError("locked ADB runtime closure must be an array")
    required.update(f"helpers/{name}" for name in runtime_files)
    missing = sorted(required - set(entries))
    if missing:
        raise EnvelopeError("checksum envelope omits required members: " + ", ".join(missing))

    actual_files = {
        path.relative_to(artifact_root).as_posix()
        for path in artifact_root.rglob("*")
        if path.name != "SHA256SUMS"
        and path.relative_to(artifact_root).parts[:1] != ("release",)
        and (path.is_file() or path.is_symlink())
    }
    unlisted = sorted(actual_files - set(entries))
    if unlisted:
        raise EnvelopeError("artifact contains unlisted members: " + ", ".join(unlisted))

    receipt = read_json(artifact_root / "receipt.json", "binary-build receipt")
    if receipt.get("receipt_kind") != "binary-build" or receipt.get("target") != expected_target:
        raise EnvelopeError("binary-build receipt is not bound to the expected target")
    if receipt.get("lock_sha256") != sha256(lock_path):
        raise EnvelopeError("binary-build receipt is not bound to the current lock")
    helper_hash = entries[f"helpers/{helper_name}"]
    if receipt.get("helper", {}).get("sha256") != helper_hash:
        raise EnvelopeError("binary-build receipt is not bound to the checksummed helper")

    for name in ("smoke.json", "package-smoke.json"):
        smoke = read_json(artifact_root / name, name)
        if smoke.get("receipt_kind") != "binary-smoke":
            raise EnvelopeError(f"{name} is not binary-smoke evidence")
        recorded_hash = smoke.get("helper_sha256")
        if recorded_hash is not None and recorded_hash != helper_hash:
            raise EnvelopeError(f"{name} is not bound to the checksummed helper")
    package = read_json(
        artifact_root / "package-verification.json", "package-verification receipt"
    )
    if package.get("receipt_kind") != "package-verification":
        raise EnvelopeError("package-verification.json has the wrong receipt kind")
    if package.get("target") != expected_target or package.get("helper_sha256") != helper_hash:
        raise EnvelopeError("package-verification receipt is not bound to the target/helper")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    parser.add_argument("--expected-target", required=True)
    args = parser.parse_args()
    try:
        verify_artifact_envelope(args.lock, args.artifact_root, args.expected_target)
        print(f"verified ADB helper checksum envelope target={args.expected_target}")
        return 0
    except EnvelopeError as error:
        print(f"ADB helper checksum envelope verification failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
