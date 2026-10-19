#!/usr/bin/env python3
"""Fail closed if a generated Intel macOS TSan O1 compile profile lost coverage.

Run after configure and before any build. This checks the generated command
ordering, not just CMake cache intent, then runs the existing sanitizer-scope
gate unchanged: first-party, mimalloc and TBB stay instrumented; other source
dependencies stay uninstrumented. Assembly entries are intentionally excluded.
"""

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


_C_FAMILY_SUFFIXES = {".c", ".C", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hpp", ".mm"}
_ASSEMBLY_SUFFIXES = {".s", ".S", ".asm"}
_DEBUG_LEVEL = re.compile(r"-g(?:[0-9]*|gdb[0-9]*|line-tables-only|line-directives-only)$")


def verify_entries(entries):
    """Return the number of checked translation units, or raise ValueError."""
    if not isinstance(entries, list) or not entries:
        raise ValueError("expected a nonempty compilation database array")
    checked = 0
    failures = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise ValueError("compilation database entry has no source filename")
        source = entry["file"]
        arguments = entry.get("arguments")
        if arguments is None:
            if not isinstance(entry.get("command"), str):
                raise ValueError(f"{source}: missing compiler command")
            arguments = shlex.split(entry["command"])
        if (not isinstance(arguments, list) or not arguments
                or not all(isinstance(arg, str) for arg in arguments)):
            raise ValueError(f"{source}: invalid compiler arguments")
        language = None
        for index, arg in enumerate(arguments):
            if arg == "-x":
                if index + 1 >= len(arguments):
                    raise ValueError(f"{source}: missing -x language")
                language = arguments[index + 1]
            elif arg.startswith("-x"):
                language = arg[2:]
        if language in ("assembler", "assembler-with-cpp"):
            continue
        suffix = Path(source).suffix
        if language is None and suffix in _ASSEMBLY_SUFFIXES:
            continue
        if ((language is not None and language not in ("c", "c++", "objective-c++"))
                or (language is None and suffix not in _C_FAMILY_SUFFIXES)):
            raise ValueError(f"{source}: unsupported compilation language")
        checked += 1
        errors = []
        optimizations = [arg for arg in arguments if arg.startswith("-O")]
        if not optimizations or optimizations[-1] != "-O1":
            errors.append("final optimization must be -O1")
        debug_levels = [arg for arg in arguments if _DEBUG_LEVEL.fullmatch(arg)]
        # Keep the project's light-debug default AND dependency-owned full
        # debug (mimalloc appends -g). The optimization hook never edits either.
        if not debug_levels or debug_levels[-1] not in ("-g1", "-g"):
            errors.append("final debug level must preserve -g1 or dependency -g")
        sanitizers = {name for arg in arguments if arg.startswith("-fsanitize=")
                      for name in arg.split("=", 1)[1].split(",")}
        if sanitizers and sanitizers != {"thread"}:
            errors.append("instrumented commands must use only -fsanitize=thread")
        for arg in arguments:
            if (arg.startswith("-fno-sanitize=")
                    and {"thread", "all"}.intersection(arg.split("=", 1)[1].split(","))):
                errors.append("thread instrumentation disabled")
            if arg.startswith(("-fno-sanitize-thread-", "-fsanitize-ignorelist=",
                               "-fsanitize-blacklist=")):
                errors.append("thread instrumentation narrowed")
            if arg == "-flto" or arg.startswith("-flto="):
                errors.append("LTO is forbidden")
            if arg.startswith("@"):
                errors.append("opaque response files are unsupported")
        if errors:
            failures.append(f"{source}: {', '.join(dict.fromkeys(errors))}")
    if not checked:
        raise ValueError("no C/C++ translation units in compilation database")
    if failures:
        raise ValueError("\n".join(failures))
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_commands", type=Path)
    args = parser.parse_args()
    try:
        entries = json.loads(args.compile_commands.read_text(encoding="utf-8"))
        checked = verify_entries(entries)
        if args.compile_commands.name != "compile_commands.json":
            raise ValueError("expected the build directory's compile_commands.json")
        scope_gate = Path(__file__).resolve().with_name("verify_sanitizer_scope.sh")
        scope = subprocess.run(["bash", str(scope_gate), str(args.compile_commands.resolve().parent)],
                               check=False)
        if scope.returncode:
            raise ValueError("existing sanitizer-scope gate rejected the compilation database")
    except (OSError, ValueError) as error:
        print(f"macOS TSan profile verification failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified {checked} C/C++ commands: final -O1, retained -g1/-g, unchanged TSan scope, no LTO")
    return 0


if __name__ == "__main__":
    sys.exit(main())
