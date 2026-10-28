import contextlib
import importlib.util
import io
from pathlib import Path, PureWindowsPath
import re
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/check_clang_tidy_scope.py"
SPEC = importlib.util.spec_from_file_location("check_clang_tidy_scope", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

LOCKED_FILTER = r".*/klogg/src/.*\.(h|hh|hpp|hxx)$"


class ClangTidyScopeTest(unittest.TestCase):
    def test_locked_filter_accepts_canonical_root_but_rejects_relocated_roots(self):
        for root, accepted in (
            ("/__w/klogg/klogg/src", True),
            ("/workspace/klogg/src", True),
            ("/workspace/klogg/.claude/worktrees/follow-pr85/src", False),
            ("/usr/local/src", False),
            ("/workspace/repo.[1]+(copy)/src", False),
        ):
            with self.subTest(root=root):
                headers = [Path(root) / ("include/example." + suffix)
                           for suffix in ("h", "hh", "hpp", "hxx")]
                if accepted:
                    MODULE.check_scope(LOCKED_FILTER, headers, Path(root))
                else:
                    with self.assertRaisesRegex(ValueError, "suppresses first-party header"):
                        MODULE.check_scope(LOCKED_FILTER, headers, Path(root))

    def test_windows_paths_use_forward_slashes_for_header_filter_matching(self):
        root = PureWindowsPath(r"C:\workspace\klogg\src")
        MODULE.check_scope(LOCKED_FILTER, [root / "include/example.h"], root)

    def test_header_paths_with_metacharacters_are_matched_as_paths(self):
        root = Path("/workspace/repo.[1]+(copy)/src")
        pattern = "^" + re.escape(root.as_posix()) + r"/.*\.(h|hh|hpp|hxx)$"
        MODULE.check_scope(pattern, [root / "include/example[1].h"], root)

    def test_dependency_headers_must_remain_excluded(self):
        root = Path("/workspace/klogg/src")
        for pattern in (".*", r".*/klogg/.*\.h$"):
            with self.subTest(pattern=pattern):
                with self.assertRaisesRegex(ValueError, "includes dependency header"):
                    MODULE.check_scope(pattern, [root / "example.h"], root)

    def test_dump_config_scalar_forms(self):
        for scalar in ("'" + LOCKED_FILTER + "'", LOCKED_FILTER):
            with self.subTest(scalar=scalar):
                self.assertEqual(MODULE.header_filter_from_dump(
                    "---\nChecks: '*'\nHeaderFilterRegex: " + scalar + "\n...\n"
                ), LOCKED_FILTER)
        self.assertEqual(MODULE.header_filter_from_dump(
            "HeaderFilterRegex: 'path''s/.*'\n"), "path's/.*")
        self.assertEqual(MODULE.header_filter_from_dump(
            'HeaderFilterRegex: "path/.*\\\\.h$"\n'), r"path/.*\.h$")

    def test_missing_ambiguous_or_unsupported_config_fails_closed(self):
        for config in ("Checks: '*'\n", "HeaderFilterRegex: |\n  .*\n",
                       "HeaderFilterRegex: *alias\n", "HeaderFilterRegex: [.*]\n",
                       "HeaderFilterRegex: 'unterminated\n",
                       "HeaderFilterRegex: .*\nHeaderFilterRegex: .*\n"):
            with self.subTest(config=config):
                with self.assertRaises(ValueError):
                    MODULE.header_filter_from_dump(config)

    def test_empty_and_invalid_filters_fail_closed(self):
        root = Path("/workspace/klogg/src")
        for pattern in ("", "["):
            with self.subTest(pattern=pattern):
                with self.assertRaises(ValueError):
                    MODULE.check_scope(pattern, [root / "example.h"], root)

    def test_main_uses_actual_source_and_header_paths_and_selected_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, expected in (("klogg", 0), ("follow-pr85", 1),
                                   ("repo.[1]+(copy)", 1)):
                with self.subTest(name=name):
                    root = Path(directory) / name / "src"
                    root.mkdir(parents=True)
                    source = root / "example.cpp"
                    source.write_text("int main() {}\n")
                    (root / "example.h").write_text("#pragma once\n")
                    result = subprocess.CompletedProcess([], 0,
                        "HeaderFilterRegex: '" + LOCKED_FILTER + "'\n", "")
                    output = io.StringIO()
                    with mock.patch.object(MODULE.subprocess, "run", return_value=result) as run, \
                            contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                        status = MODULE.main(["--source-root", str(root),
                                              "--clang-tidy", "/tools/clang-tidy-18"])
                    self.assertEqual(status, expected, output.getvalue())
                    self.assertEqual(run.call_args.args[0],
                                     ["/tools/clang-tidy-18", "--dump-config", str(source.resolve())])
                    if expected:
                        self.assertIn("canonical", output.getvalue())
                        self.assertIn("requalification", output.getvalue())

    def test_missing_tool_and_dump_failure_are_not_clean(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "example.cpp").write_text("int main() {}\n")
            (root / "example.h").write_text("#pragma once\n")
            for error in (FileNotFoundError("missing clang-tidy"),
                          subprocess.CalledProcessError(1, "clang-tidy", stderr="invalid config")):
                with self.subTest(error=error), \
                        mock.patch.object(MODULE.subprocess, "run", side_effect=error), \
                        contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(MODULE.main(["--source-root", str(root)]), 1)

    def test_empty_source_root_fails_before_invoking_tool(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(MODULE.subprocess, "run") as run, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(MODULE.main(["--source-root", directory]), 1)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
