"""iOS core builds fail before compilation on an unreviewed Xcode/SDK host."""

import contextlib
import hashlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import ci_dependency_catalog as catalog
import ci_dependency_toolchain as toolchain


class IosProducerToolchainTest(unittest.TestCase):
    def setUp(self):
        self.outputs = {
            ("xcodebuild", "-version"): "Xcode 26.6\nBuild version 17F113\n",
            ("xcrun", "--show-sdk-version"): "26.5\n",
            ("xcrun", "--show-sdk-path"): "/Applications/Xcode_26.6.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk\n",
            ("clang", "--version"): "Apple clang version 21.0.0 (clang-2100.1.1.101)\nTarget: arm64-apple-darwin\n",
            ("cmake", "--version"): "cmake version 3.31.6\n",
            ("ninja", "--version"): "1.12.1\n",
        }
        self.commands = []

    def fake_run(self, command, **kwargs):
        self.commands.append((tuple(command), kwargs))
        return subprocess.CompletedProcess(command, 0, self.outputs[tuple(command)], "")

    def check(self, *, target="ios-arm64", arch="arm64", developer_dir=None):
        if developer_dir is None:
            developer_dir = "/Applications/Xcode_26.6.app/Contents/Developer"
        return toolchain.check_ios_toolchain(
            target_id=target, repo_root=ROOT, host_arch=arch,
            developer_dir=developer_dir, run_command=self.fake_run,
        )

    def test_selected_xcode_and_observed_tools_match_reviewed_catalog(self):
        self.assertEqual(self.check(), catalog.IOS_TOOLCHAIN)
        self.assertEqual([command for command, _ in self.commands], list(self.outputs))
        self.assertTrue(all(options["check"] and options["timeout"] == 30
                            and options["capture_output"] and options["text"]
                            for _, options in self.commands))
        self.assertEqual(self.check(target="ios-x86_64", arch="x86_64"),
                         catalog.IOS_TOOLCHAIN)

    def test_unreviewed_host_probe_records_selected_tools_and_missing_formula(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            names = ("autoreconf", "autoconf", "automake", "aclocal", "glibtoolize",
                     "pkg-config", "perl", "m4")
            tools = {}
            for name in names:
                path = root / name
                path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                path.chmod(0o755)
                tools[name] = str(path)
            calls = []

            def fake(command, **options):
                calls.append(tuple(command))
                if command[:3] == ["brew", "list", "--versions"]:
                    formula = command[3]
                    if formula == "m4":
                        return subprocess.CompletedProcess(command, 1, "", "not installed")
                    return subprocess.CompletedProcess(command, 0, formula + " 1.0\n", "")
                return subprocess.CompletedProcess(command, 0, "\n" + pathlib.Path(command[0]).name + " version 1.0\n", "")

            observed = toolchain.observe_ios_host_tools(
                target_id="ios-x86_64", host_arch="x86_64",
                developer_dir=toolchain.DEVELOPER_DIR,
                find_executable=tools.get, run_command=fake,
            )
            self.assertEqual(observed["kind"], "unreviewed-ios-host-tools")
            self.assertEqual(observed["target_id"], "ios-x86_64")
            for name in names:
                self.assertEqual(observed["tools"][name], {
                    "path": tools[name], "resolved_path": str(pathlib.Path(tools[name]).resolve()),
                    "sha256": hashlib.sha256(pathlib.Path(tools[name]).read_bytes()).hexdigest(),
                    "version": name + " version 1.0",
                })
            self.assertIsNone(observed["homebrew"]["m4"])
            self.assertEqual(observed["homebrew"]["pkgconf"], "pkgconf 1.0")
            self.assertIn(("brew", "list", "--versions", "m4"), calls)
            self.assertNotIn("qualification", observed)

    def test_host_probe_rejects_wrong_arch_and_missing_tools(self):
        for target, arch in (("ios-arm64", "x86_64"), ("ios-x86_64", "x86_64")):
            with self.subTest(target=target), self.assertRaises(toolchain.ToolchainError):
                toolchain.observe_ios_host_tools(
                    target_id=target, host_arch=arch,
                    developer_dir=toolchain.DEVELOPER_DIR,
                    find_executable=lambda _: None, run_command=self.fake_run,
                )
            self.assertEqual(self.commands, [])

    def test_dependency_probe_logs_observation_but_cannot_qualify_without_review(self):
        output = io.StringIO()
        diagnostic = io.StringIO()
        observed = {"schema_version": 1, "kind": "unreviewed-ios-host-tools",
                    "target_id": "ios-x86_64", "tools": {"m4": {"version": "1.4.21"}}}
        with mock.patch.dict(os.environ, {"DEVELOPER_DIR": toolchain.DEVELOPER_DIR}), \
                mock.patch.object(toolchain.platform, "machine", return_value="x86_64"), \
                mock.patch.object(toolchain, "check_ios_toolchain", return_value=catalog.IOS_TOOLCHAIN), \
                mock.patch.object(toolchain, "observe_ios_host_tools", return_value=observed) as probe, \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(diagnostic), \
                self.assertRaises(SystemExit) as failure:
            toolchain.main(["--target-id", "ios-x86_64", "--probe-unreviewed-host-tools"])
        self.assertEqual(failure.exception.code, 1)
        self.assertIn("unreviewed", diagnostic.getvalue())
        self.assertEqual(json.loads(output.getvalue()), observed)
        probe.assert_called_once()

    def test_wrong_host_architecture_or_xcode_selection_fails_before_commands(self):
        for arguments in ({"target": "ios-arm64", "arch": "x86_64"},
                          {"target": "ios-x86_64", "arch": "arm64"},
                          {"developer_dir": "/Applications/Xcode_26.5.app/Contents/Developer"},
                          {"target": "adb-macos-arm64"}):
            with self.subTest(arguments=arguments), self.assertRaises(toolchain.ToolchainError):
                self.check(**arguments)
            self.assertEqual(self.commands, [])

    def test_any_changed_observed_tool_refuses_native_qualification(self):
        for command, output in (
            (("xcodebuild", "-version"), "Xcode 26.5\nBuild version 26F6\n"),
            (("xcrun", "--show-sdk-version"), "26.4\n"),
            (("xcrun", "--show-sdk-path"), "/Applications/Xcode_26.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk\n"),
            (("clang", "--version"), "Apple clang version 18.0.0\n"),
            (("cmake", "--version"), "cmake version 4.4.2\n"),
            (("ninja", "--version"), "1.12.0\n"),
        ):
            self.commands.clear()
            previous = self.outputs[command]
            self.outputs[command] = output
            with self.subTest(command=command), self.assertRaises(toolchain.ToolchainError):
                self.check()
            self.outputs[command] = previous


if __name__ == "__main__":
    unittest.main()
