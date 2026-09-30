"""Exercise first-chance Windows diagnostics in a disposable synthetic process."""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

MAX_DUMP_BYTES = 32 * 1024 * 1024
MAX_EVIDENCE_TEXT_BYTES = 64 * 1024


def exception_from_dump(path: pathlib.Path) -> dict:
    if path.stat().st_size > MAX_DUMP_BYTES:
        raise ValueError("test minidump exceeds smoke-test budget")
    content = path.read_bytes()
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
            # Only native base layouts are supported. DataOffset/P1Home can alias
            # architecture flags at the other layout's offset, so size comes first.
            if context_size == 1232:
                width, registers, flag_offset, architecture = "<Q", (248, 152, 160), 48, 0x100000
            elif context_size == 716:
                width, registers, flag_offset, architecture = "<I", (184, 196, 180), 0, 0x10000
            else:
                flags0 = (f"0x{struct.unpack_from('<I', content, context_offset)[0]:08x}"
                          if context_size >= 4 else "unavailable")
                flags48 = (f"0x{struct.unpack_from('<I', content, context_offset + 48)[0]:08x}"
                           if context_size >= 52 else "unavailable")
                raise ValueError(f"test minidump context layout unsupported: size {context_size} "
                                 f"RVA {context_offset} flags0 {flags0} flags48 {flags48}")
            flags = struct.unpack_from("<I", content, context_offset + flag_offset)[0]
            if flags & 0x00FF0000 != architecture:
                raise ValueError("test minidump context architecture missing")
            pc, sp, fp = [struct.unpack_from(width, content, context_offset + register)[0]
                          for register in registers]
            return {"thread": thread, "code": code, "address": address,
                    "parameters": parameters, "operation": operation, "accessed": accessed,
                    "pc": pc, "sp": sp, "fp": fp}
    raise ValueError("test minidump has no exception stream")


def retain_failed_smoke(output: pathlib.Path, parent: str | None, error: Exception) -> None:
    if not parent:
        return
    destination = pathlib.Path(parent)
    if not destination.is_dir() or destination.is_symlink():
        return
    try:
        retained = pathlib.Path(tempfile.mkdtemp(prefix="windows-crash-smoke-", dir=destination))
        (retained / "failure.txt").write_bytes(str(error).encode("utf-8")[:MAX_EVIDENCE_TEXT_BYTES])
        stderr = output / "helper-stderr.txt"
        if stderr.is_file():
            (retained / stderr.name).write_bytes(stderr.read_bytes()[:MAX_EVIDENCE_TEXT_BYTES])
        remaining = MAX_DUMP_BYTES
        for index, dump in enumerate(output.glob("*.dmp")):
            if index >= 2:
                break
            size = dump.stat().st_size
            if size <= remaining:
                shutil.copyfile(dump, retained / dump.name)
                remaining -= size
        print(f"Synthetic crash smoke evidence retained at {retained}", file=sys.stderr)
    except OSError as retention_error:
        print(f"Synthetic crash smoke evidence retention failed: {retention_error}", file=sys.stderr)


def run_smoke_case(helper: pathlib.Path, output: pathlib.Path, case: str) -> None:
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("KLOGG_TEST_MINIDUMP_DIR", "_NT_SYMBOL_PATH")}
    if case == "enabled":
        environment["KLOGG_TEST_MINIDUMP_DIR"] = str(output)
    elif case == "invalid":
        environment["KLOGG_TEST_MINIDUMP_DIR"] = str(output / "missing")
    result = subprocess.run([str(helper)], env=environment, cwd=output,
                            capture_output=True, text=True, timeout=30)
    (output / "helper-stderr.txt").write_bytes(result.stderr.encode("utf-8")[:MAX_EVIDENCE_TEXT_BYTES])
    if result.returncode != 67:
        raise ValueError("crash probe failed to preserve exception handling: " + result.stderr)
    if result.stderr.count("Exception address ") != 2:
        raise ValueError("repeated first-chance exception evidence missing: " + result.stderr)
    dumps = list(output.glob("*.dmp"))
    if case != "enabled":
        if dumps:
            raise ValueError("disabled capture unexpectedly wrote a dump")
        return
    if len(dumps) != 1 or "Test minidump captured 1 error 0" not in result.stderr:
        raise ValueError("one minimal test dump was not captured: " + result.stderr)
    exception = exception_from_dump(dumps[0])
    first = re.search(r"Exception address ([0-9a-fA-Fx]+) thread ([0-9]+)", result.stderr)
    context = re.search(r"Original PC 0x([0-9a-fA-F]+) SP 0x([0-9a-fA-F]+) FP 0x([0-9a-fA-F]+)",
                        result.stderr)
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
    parent = os.environ.get("KLOGG_TEST_MINIDUMP_DIR")
    with tempfile.TemporaryDirectory(prefix="klogg-crash-probe-") as directory:
        root = pathlib.Path(directory)
        for case in ("enabled", "absent", "invalid"):
            output = root / case
            output.mkdir()
            try:
                run_smoke_case(helper, output, case)
            except Exception as error:
                retain_failed_smoke(output, parent, error)
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
