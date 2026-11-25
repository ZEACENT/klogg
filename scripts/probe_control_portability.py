#!/usr/bin/env python3
"""Fast local control portability probe (Clang, configured build; no outputs).

Run on macOS before pushing control changes:
  python3 scripts/probe_control_portability.py --qt5-prefix "$(brew --prefix qt@5)"
It checks the document callback under GCC-equivalent lexical shadow diagnostics,
RPC with finite-math flags, and the client sources, schema test and GUI test with Qt 5.
A configured Qt 5 build may instead be supplied via --build-dir.
"""
import argparse
from pathlib import Path
import subprocess
import sys

from lint_header_self_contained import first_party_units, load_compile_database, syntax_only_command


ROOT = Path(__file__).resolve().parents[1]
PROBES = {
    "src/ui/src/reviewdocumentadapter.cpp": ["-Werror=shadow-uncaptured-local"],
    "src/control/src/controlrpc.cpp": ["-ffinite-math-only"],
    "src/control/src/controlframing.cpp": [],
    "src/control/src/controlschema.cpp": [],
    "tests/unit/controlschema_test.cpp": [],
    "tests/ui/reviewcontroladapter_test.cpp": [],
}


def qt5_command(command, prefix):
    # Replace the SDK paths from the configured Homebrew framework toolchain,
    # preserving all first-party, generated and third-party include flags.
    core = next((Path(arg) for arg in command if arg.endswith("/lib/QtCore.framework/Headers")), None)
    if core is None:
        raise ValueError("--qt5-prefix requires a configured Qt framework build")
    original = str(core.parents[2]) + "/"
    return [arg.replace(original, str(prefix) + "/", 1).replace("/share/qt/mkspecs/", "/mkspecs/")
            if arg.startswith(original) else arg for arg in command if arg != "-DQT_CORE5COMPAT_LIB"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build_root")
    parser.add_argument("--qt5-prefix", type=Path, help="Homebrew Qt 5 prefix when probing from a Qt 6 build")
    args = parser.parse_args()
    database = load_compile_database(args.build_dir)
    units = {unit[0]: unit for unit in first_party_units(database, ROOT)}
    for name, flags in PROBES.items():
        path = ROOT / name
        if path not in units:
            raise ValueError("missing compile command for " + name)
        unit = units[path]
        command = syntax_only_command(unit, path, flags)
        if args.qt5_prefix:
            command = qt5_command(command, args.qt5_prefix)
        # Verify the selected headers, not just the presence of a Qt 5 prefix.
        # Use stdin and an absolute include to preserve quoted-include lookup.
        command[-1] = "-"
        probe = ("#include <QtGlobal>\nstatic_assert(QT_VERSION_MAJOR == 5, \"Qt 5 headers required\");\n"
                 f'#include "{path.as_posix()}"\n')
        print("Checking " + name, flush=True)
        subprocess.run(command, input=probe, text=True, cwd=unit[1], check=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
