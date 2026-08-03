import importlib.util
import pathlib
import re
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "run_changed_clang_tidy.py"
SPEC = importlib.util.spec_from_file_location("run_changed_clang_tidy", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ChangedClangTidyTest(unittest.TestCase):
    def test_local_diff_includes_staged_and_unstaged_changes(self):
        root = pathlib.Path("/workspace")
        with mock.patch.object(
            MODULE, "git_output", return_value=b"src/example.cpp\0"
        ) as git_output:
            self.assertEqual(
                MODULE.changed_paths(root, "base-sha"),
                [pathlib.Path("src/example.cpp")],
            )

        git_output.assert_called_once_with(
            root,
            "-c",
            "core.quotePath=false",
            "diff",
            "--name-only",
            "-z",
            "base-sha",
            "--",
            "src/",
        )

    def test_unified_patch_includes_staged_and_unstaged_changes(self):
        root = pathlib.Path("/workspace")
        with mock.patch.object(MODULE, "git_output", return_value=b"patch") as git_output:
            self.assertEqual(MODULE.unified_patch(root, "base-sha"), "patch")

        git_output.assert_called_once_with(
            root,
            "-c",
            "core.quotePath=false",
            "diff",
            "-U0",
            "base-sha",
            "--",
            "src/",
        )

    def test_untracked_source_files_fail_closed(self):
        root = pathlib.Path("/workspace")
        with mock.patch.object(
            MODULE, "git_output", return_value=b"src/new.CPP\0"
        ):
            with self.assertRaisesRegex(ValueError, "stage or add untracked"):
                MODULE.ensure_no_untracked_cpp_paths(root)

    def test_changed_sources_require_exact_compile_database_entries(self):
        root = pathlib.Path("/workspace")
        units = [root / "src" / "configured.cpp"]
        MODULE.require_configured_sources(
            root, [pathlib.Path("src/configured.cpp")], units
        )
        with self.assertRaisesRegex(ValueError, "not configured"):
            MODULE.require_configured_sources(
                root, [pathlib.Path("src/missing.cpp")], units
            )

    def test_direct_header_command_forces_the_changed_header_to_be_parsed(self):
        command = MODULE.header_analysis_command(
            pathlib.Path("/usr/bin/clang-tidy"),
            pathlib.Path("/workspace/build"),
            pathlib.Path("/workspace/src/example.H"),
            '[{"name":"src/example.H","lines":[[3,3]]}]',
            ["-nostdinc++"],
        )
        self.assertEqual(command[-1], "/workspace/src/example.H")
        self.assertIn("--line-filter=[{\"name\":\"src/example.H\",\"lines\":[[3,3]]}]", command)
        self.assertIn("-extra-arg=-nostdinc++", command)

    def test_source_regex_accepts_uppercase_extensions(self):
        self.assertRegex("src/example.CPP", MODULE.SOURCE_FILE_RE)

    def test_source_iregex_compiles_after_driver_prefix_wrap(self):
        # clang-tidy-diff.py wraps the iregex as "^%s$"; an inline (?i) flag
        # then sits mid-pattern, which Python 3.14 rejects outright.
        wrapped = "^%s$" % MODULE.CLANG_TIDY_SOURCE_IREGEX
        compiled = re.compile(wrapped, re.IGNORECASE)
        self.assertIsNotNone(compiled.match("src/example.CPP"))
        self.assertIsNone(compiled.match("src/example.hpp"))

    def test_finding_filter_keeps_only_changed_paths_and_deduplicates(self):
        changed = pathlib.Path("src/logdata/src/capturestore.cpp")
        finding = (
            "/workspace/src/logdata/src/capturestore.cpp:10:4: "
            "warning: problem [check-name]"
        )
        output = "\n".join(
            [
                finding,
                finding,
                "/workspace/src/other.cpp:20:2: warning: old problem [check-name]",
                "tool warning: not a source diagnostic",
            ]
        )

        self.assertEqual(MODULE.finding_lines(output, [changed]), [finding])

    def test_finding_filter_accepts_error_diagnostics(self):
        changed = pathlib.Path("src/logdata/src/securecapturedirectory.cpp")
        finding = (
            "src/logdata/src/securecapturedirectory.cpp:5:1: "
            "error: analysis failed [check-name]"
        )

        self.assertEqual(MODULE.finding_lines(finding, [changed]), [finding])

    def test_clang_tidy_extra_arguments_are_forwarded_individually(self):
        self.assertEqual(
            MODULE.clang_tidy_extra_arguments(
                ["-nostdinc++", "-isystem", "/opt/llvm/include/c++/v1"]
            ),
            [
                "-extra-arg=-Wno-unknown-warning-option",
                "-extra-arg=-nostdinc++",
                "-extra-arg=-isystem",
                "-extra-arg=/opt/llvm/include/c++/v1",
            ],
        )

    def test_unsupported_control_characters_are_explicit(self):
        for character in ("\n", "\x1b", "\x7f", "\\", '"'):
            with self.subTest(character=repr(character)):
                self.assertTrue(
                    MODULE.has_unsupported_path_character(
                        pathlib.Path(f"src/file{character}.cpp")
                    )
                )
        self.assertFalse(
            MODULE.has_unsupported_path_character(pathlib.Path("src/ümlaut.cpp"))
        )


if __name__ == "__main__":
    unittest.main()
