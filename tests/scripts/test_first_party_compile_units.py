import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "first_party_compile_units.py"
SPEC = importlib.util.spec_from_file_location("first_party_compile_units", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class FirstPartyCompileUnitsTest(unittest.TestCase):
    def test_uses_only_configured_translation_units_under_source_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_root = root / "src"
            source_root.mkdir()
            configured = source_root / "ui" / "configured.cpp"
            configured.parent.mkdir()
            configured.write_text("")
            unconfigured = source_root / "crash_handler" / "unconfigured.cpp"
            unconfigured.parent.mkdir()
            unconfigured.write_text("")
            dependency = root / "cpm_cache" / "dependency.cpp"
            dependency.parent.mkdir()
            dependency.write_text("")
            generated = root / "build" / "autogen.cpp"
            generated.parent.mkdir()
            generated.write_text("")

            database = [
                {"directory": str(root), "file": "src/ui/configured.cpp"},
                {"directory": str(root), "file": str(dependency)},
                {"directory": str(root), "file": str(generated)},
            ]

            self.assertEqual(
                MODULE.first_party_compile_units(database, source_root),
                [configured.resolve()],
            )
            self.assertNotIn(unconfigured.resolve(), MODULE.first_party_compile_units(database, source_root))

    def test_loads_compile_database_and_sorts_unique_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_root = root / "src"
            source_root.mkdir()
            first = source_root / "a.cpp"
            second = source_root / "b.cxx"
            first.write_text("")
            second.write_text("")
            database_path = root / "compile_commands.json"
            database_path.write_text(
                json.dumps(
                    [
                        {"directory": str(root), "file": str(second)},
                        {"directory": str(root), "file": str(first)},
                        {"directory": str(root), "file": str(second)},
                    ]
                )
            )

            self.assertEqual(
                MODULE.load_first_party_compile_units(database_path, source_root),
                [first.resolve(), second.resolve()],
            )

    def test_null_output_is_safe_for_paths_with_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_root = root / "source tree"
            source_root.mkdir()
            configured = source_root / "file with spaces.cpp"
            configured.write_text("")
            database_path = root / "compile_commands.json"
            database_path.write_text(
                json.dumps([{"directory": str(root), "file": str(configured)}])
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(database_path),
                    str(source_root),
                    "--null",
                ],
                check=False,
                capture_output=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout, os.fsencode(configured.resolve()) + b"\0")
            self.assertEqual(result.stderr, b"")


if __name__ == "__main__":
    unittest.main()
