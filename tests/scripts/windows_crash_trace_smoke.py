"""Exercise first-chance Windows diagnostics in a disposable synthetic process."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import pe_identity

MAX_DUMP_BYTES = 32 * 1024 * 1024
MAX_EVIDENCE_TEXT_BYTES = 64 * 1024
MAX_EVIDENCE_SUMMARY_BYTES = 64 * 1024
MAX_DIAGNOSTIC_TEXT_BYTES = 8 * 1024
DIAGNOSTIC_PREFIX = "Test minidump diagnostic "
MAX_PE_HEADER_OFFSET = pe_identity.MAX_PE_HEADER_OFFSET


def architecture_from_pe(path: pathlib.Path) -> str:
    try:
        architecture = pe_identity.architecture_from_pe(path)
    except ValueError as error:
        raise ValueError("test helper " + str(error)) from error
    if architecture != "x64":
        raise ValueError("test helper PE architecture unsupported: " + architecture)
    return architecture


def exception_from_dump(path: pathlib.Path, process_architecture: str) -> dict:
    if process_architecture != "x64":
        raise ValueError("test minidump process architecture unsupported")
    size = path.stat().st_size
    if size == 0:
        raise ValueError("test minidump header missing: empty output")
    if size > MAX_DUMP_BYTES:
        raise ValueError("test minidump exceeds smoke-test budget")
    with path.open("rb") as stream:
        # Bound the read itself, including if a file grows after the size check.
        content = stream.read(size)
    if len(content) < 32 or content[:4] != b"MDMP":
        raise ValueError("test minidump header missing")
    count, directory = struct.unpack_from("<II", content, 8)
    if not 0 < count <= 128 or directory + count * 12 > len(content):
        raise ValueError("test minidump directory invalid")
    for index in range(count):
        kind, size, offset = struct.unpack_from("<III", content, directory + index * 12)
        if offset + size > len(content):
            raise ValueError("test minidump stream outside file")
        if kind == 6:
            if size < 168:
                raise ValueError("test minidump exception stream incomplete")
            thread, code = struct.unpack_from("<I4xI", content, offset)
            address = struct.unpack_from("<Q", content, offset + 24)[0]
            parameters = struct.unpack_from("<I", content, offset + 32)[0]
            operation, accessed = struct.unpack_from("<QQ", content, offset + 40)
            context_size, context_offset = struct.unpack_from("<II", content, offset + 160)
            if not context_size or context_offset + context_size > len(content):
                raise ValueError("test minidump original context missing")
            # XSTATE extends the native base by a variable amount. Decode only
            # the documented x64 prefix after validating the process identity;
            # trailing state stays opaque because this smoke checks PC/SP/FP only.
            if context_size < 1232:
                flags0 = (f"0x{struct.unpack_from('<I', content, context_offset)[0]:08x}"
                          if context_size >= 4 else "unavailable")
                flags48 = (f"0x{struct.unpack_from('<I', content, context_offset + 48)[0]:08x}"
                           if context_size >= 52 else "unavailable")
                raise ValueError(f"test minidump context layout unsupported: size {context_size} "
                                 f"RVA {context_offset} flags0 {flags0} flags48 {flags48}")
            flags = struct.unpack_from("<I", content, context_offset + 48)[0]
            if (flags & 0x00FF0000) != 0x100000:
                raise ValueError("test minidump context architecture missing")
            if (flags & 3) != 3:
                raise ValueError("test minidump context required register flags missing")
            pc, sp, fp = [struct.unpack_from("<Q", content, context_offset + register)[0]
                          for register in (248, 152, 160)]
            return {"thread": thread, "code": code, "address": address,
                    "parameters": parameters, "operation": operation, "accessed": accessed,
                    "pc": pc, "sp": sp, "fp": fp}
    raise ValueError("test minidump has no exception stream")


def bounded_text(text: str, maximum: int) -> str:
    return text[:maximum].encode("utf-8", errors="replace")[:maximum].decode("utf-8", errors="ignore")


def helper_identity(helper: pathlib.Path | None, architecture: str | None) -> dict | None:
    if helper is None:
        return None
    identity = {"path": bounded_text(str(helper), 384), "architecture": architecture,
                "observed_size_bytes": None, "sha256": None, "status": "unavailable"}
    if helper.is_symlink() or not helper.is_file():
        return identity
    size = helper.stat().st_size
    identity["observed_size_bytes"] = size
    if size > MAX_DUMP_BYTES:
        identity["status"] = "over-budget"
        return identity
    digest = hashlib.sha256()
    remaining = size
    with helper.open("rb") as stream:
        while remaining:
            content = stream.read(min(remaining, 64 * 1024))
            if not content:
                identity["status"] = "short-read"
                return identity
            digest.update(content)
            remaining -= len(content)
    identity.update(sha256=digest.hexdigest(), status="observed-file-prefix")
    return identity


def retain_failed_smoke(output: pathlib.Path, parent: str | None, error: Exception,
                        helper: pathlib.Path | None = None,
                        architecture: str | None = None) -> None:
    if not parent:
        return
    destination = pathlib.Path(parent)
    if not destination.is_dir() or destination.is_symlink():
        return
    try:
        retained = pathlib.Path(tempfile.mkdtemp(prefix="windows-crash-smoke-", dir=destination))
        (retained / "failure.txt").write_bytes(str(error).encode("utf-8", errors="replace")[:MAX_EVIDENCE_TEXT_BYTES])
        stderr = output / "helper-stderr.txt"
        stderr_bytes = b""
        if not stderr.is_symlink() and stderr.is_file():
            with stderr.open("rb") as stream:
                stderr_bytes = stream.read(MAX_EVIDENCE_TEXT_BYTES)
            (retained / stderr.name).write_bytes(stderr_bytes)
        # The native JSON record is evidence, not a command, path authority, or
        # replacement success signal. Keep its bounded text opaque here.
        diagnostics = []
        diagnostic_remaining = MAX_DIAGNOSTIC_TEXT_BYTES
        diagnostics_truncated = False
        for line in stderr_bytes.decode("utf-8", errors="replace").splitlines():
            if line.startswith(DIAGNOSTIC_PREFIX):
                bounded = bounded_text(line, diagnostic_remaining)
                if bounded:
                    diagnostics.append(bounded)
                    diagnostic_remaining -= len(bounded.encode("utf-8"))
                diagnostics_truncated |= bounded != line
        record = {"schema_version": 1, "case": bounded_text(output.name, 128),
                  "failure": bounded_text(str(error), 512),
                  "provenance": {"github_environment": {
                      key: bounded_text(os.environ[key], 96)
                      for key in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT",
                                  "GITHUB_JOB", "GITHUB_REPOSITORY", "GITHUB_WORKFLOW")
                      if key in os.environ},
                      "helper": helper_identity(helper, architecture)},
                  "diagnostics": diagnostics, "diagnostics_truncated": diagnostics_truncated,
                  "dumps": []}
        remaining = MAX_DUMP_BYTES
        for index, dump in enumerate(output.glob("*.dmp")):
            if index >= 2:
                break
            metadata = {"name": bounded_text(dump.name, 128), "observed_size_bytes": None,
                        "retained_size_bytes": None, "sha256": None, "hash_scope": None,
                        "status": "not-regular"}
            record["dumps"].append(metadata)
            if dump.is_symlink() or not dump.is_file():
                continue
            size = dump.stat().st_size
            metadata["observed_size_bytes"] = size
            if size > remaining:
                metadata["status"] = "over-budget"
                continue
            digest = hashlib.sha256()
            copied = 0
            with dump.open("rb") as source, (retained / dump.name).open("xb") as target:
                while copied < size:
                    content = source.read(min(size - copied, 64 * 1024))
                    if not content:
                        break
                    target.write(content)
                    digest.update(content)
                    copied += len(content)
            remaining -= size
            metadata.update(retained_size_bytes=copied, sha256=digest.hexdigest(),
                            hash_scope="retained-bytes",
                            status="retained" if copied == size else "short-read")
        summary = json.dumps(record, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        if len(summary) > MAX_EVIDENCE_SUMMARY_BYTES:
            record.update(diagnostics=[], diagnostics_truncated=True)
            summary = json.dumps(record, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        if len(summary) <= MAX_EVIDENCE_SUMMARY_BYTES:
            (retained / "failure-summary.json").write_bytes(summary)
        print(f"Synthetic crash smoke evidence retained at {retained}", file=sys.stderr)
    except OSError as retention_error:
        print(f"Synthetic crash smoke evidence retention failed: {retention_error}", file=sys.stderr)


def run_smoke_case(helper: pathlib.Path, output: pathlib.Path, case: str,
                   architecture: str) -> None:
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("KLOGG_TEST_MINIDUMP_DIR", "_NT_SYMBOL_PATH")}
    if case in ("enabled", "idle"):
        environment["KLOGG_TEST_MINIDUMP_DIR"] = str(output)
    elif case == "invalid":
        environment["KLOGG_TEST_MINIDUMP_DIR"] = str(output / "missing")
    elif case == "non-directory":
        non_directory = output / "not-directory.txt"
        non_directory.write_bytes(b"Not a directory\n")
        environment["KLOGG_TEST_MINIDUMP_DIR"] = str(non_directory)
    command = [str(helper)] + (["--idle"] if case == "idle" else [])
    result = subprocess.run(command, env=environment, cwd=output,
                            capture_output=True, text=True, timeout=30)
    (output / "helper-stderr.txt").write_bytes(result.stderr.encode("utf-8")[:MAX_EVIDENCE_TEXT_BYTES])
    # Optional diagnostic fields are opaque data. Embedded marker text can
    # neither supply missing functional evidence nor invalidate real evidence.
    trace_lines = [line for line in result.stderr.splitlines()
                   if not line.startswith(DIAGNOSTIC_PREFIX)]
    trace = "\n".join(trace_lines)
    if case == "idle":
        if (result.returncode != 0 or list(output.glob("*.dmp"))
                or trace_lines.count("Test minidump collector ready") != 1
                or trace_lines.count("Test minidump collector stopped") != 1
                or "Test minidump captured " in trace
                or "Exception address " in trace):
            raise ValueError("idle collector did not complete startup and cleanup: " + result.stderr)
        return
    if result.returncode != 67:
        raise ValueError("crash probe failed to preserve exception handling: " + result.stderr)
    if len(re.findall(r"^Exception address [0-9a-fA-Fx]+ thread [0-9]+$", trace, re.MULTILINE)) != 2:
        raise ValueError("repeated first-chance exception evidence missing: " + result.stderr)
    dumps = list(output.glob("*.dmp"))
    if case != "enabled":
        if dumps:
            raise ValueError("disabled capture unexpectedly wrote a dump")
        if "Test minidump collector ready" in trace or "Test minidump captured " in trace:
            raise ValueError("disabled capture unexpectedly launched a collector: " + result.stderr)
        if case == "invalid" and (output / "missing").exists():
            raise ValueError("invalid opt-in unexpectedly created a directory")
        if case == "non-directory" and non_directory.read_bytes() != b"Not a directory\n":
            raise ValueError("invalid opt-in changed an existing non-directory file")
        return
    if (len(dumps) != 1 or dumps[0].is_symlink() or not dumps[0].is_file()
            or trace_lines.count("Test minidump captured 1 error 0") != 1
            or trace.count("Test minidump captured ") != 1
            or trace_lines.count("Test minidump collector ready") != 1):
        raise ValueError("one minimal test dump was not captured: " + result.stderr)
    exception = exception_from_dump(dumps[0], architecture)
    first = re.search(r"^Exception address ([0-9a-fA-Fx]+) thread ([0-9]+)$", trace, re.MULTILINE)
    context = re.search(r"^Original PC 0x([0-9a-fA-F]+) SP 0x([0-9a-fA-F]+) FP 0x([0-9a-fA-F]+)$",
                        trace, re.MULTILINE)
    if (first is None or context is None
            or exception["address"] != int(first[1], 16)
            or exception["thread"] != int(first[2])
            or [exception[key] for key in ("pc", "sp", "fp")]
            != [int(value, 16) for value in context.groups()]):
        raise ValueError("test minidump differs from original exception context")
    if (exception["code"] != 0xC0000005 or exception["parameters"] != 2
            or exception["accessed"] != 0x1234 or exception["operation"] != 0
            or not exception["thread"] or not exception["address"]):
        raise ValueError("test dump lost original synthetic exception information")


def run_smoke(helper: pathlib.Path) -> None:
    architecture = architecture_from_pe(helper)
    parent = os.environ.get("KLOGG_TEST_MINIDUMP_DIR")
    with tempfile.TemporaryDirectory(prefix="klogg-crash-probe-") as directory:
        root = pathlib.Path(directory)
        for case in ("enabled", "absent", "invalid", "non-directory", "idle"):
            output = root / case
            output.mkdir()
            try:
                run_smoke_case(helper, output, case, architecture)
            except Exception as error:
                retain_failed_smoke(output, parent, error, helper, architecture)
                raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("Windows crash probe requires a Windows host")
    run_smoke(args.helper.resolve(strict=True))
    print("Windows first-chance context, one minimal dump, and disabled capture verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
