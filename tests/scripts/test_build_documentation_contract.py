from __future__ import annotations

import pathlib
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]


def test_instructions():
    documentation = (ROOT / "docs" / "BUILD.md").read_text(encoding="utf-8")
    return documentation.split("## Running tests\n", 1)[1].split("### ", 1)[0]


class BuildDocumentationContractTest(unittest.TestCase):
    def test_ctest_options_support_the_declared_cmake_minimum(self):
        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"cmake_minimum_required\(VERSION\s+(\d+)\.(\d+)", source)
        self.assertIsNotNone(match, "The source must declare its CMake minimum")
        minimum = tuple(map(int, match.groups()))
        if minimum < (3, 20):
            self.assertNotIn(
                "--test-dir",
                test_instructions(),
                "CTest --test-dir needs CMake 3.20; use cmake -E chdir for the supported floor",
            )

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "CMake and CTest required")
    def test_shared_ctest_examples_are_portable_and_executable(self):
        instructions = test_instructions()
        commands = re.findall(
            r"^((?:cmake -E chdir build_root )?ctest [^\n]+)$", instructions, re.MULTILINE
        )
        self.assertGreaterEqual(len(commands), 2, "Show serial and parallel CTest examples")
        self.assertNotIn("getconf", instructions, "Shared test instructions must also work on Windows")
        self.assertTrue(any("--parallel" in shlex.split(command) for command in commands))

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = root / "build_root"
            build.mkdir()
            cmake = pathlib.Path(shutil.which("cmake")).as_posix()
            (build / "CTestTestfile.cmake").write_text(
                f'add_test(documentation-smoke "{cmake}" "-E" "echo" "documentation smoke")\n',
                encoding="utf-8",
            )
            for command in commands:
                with self.subTest(command=command):
                    args = shlex.split(command)
                    self.assertEqual(args[:5], ["cmake", "-E", "chdir", "build_root", "ctest"])
                    self.assertIn("--build-config", args)
                    self.assertEqual(args[args.index("--build-config") + 1], "RelWithDebInfo")
                    result = subprocess.run(
                        args, cwd=root, capture_output=True, text=True, check=False, timeout=30
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("100% tests passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
