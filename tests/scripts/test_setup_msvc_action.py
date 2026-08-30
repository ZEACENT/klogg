from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
SETUP_MSVC = ROOT / ".github" / "actions" / "setup-msvc" / "action.yml"
LINT = ROOT / "scripts" / "lint_ci_quality.py"
LEGACY_ACTION = "ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756"


class SetupMsvcActionContractTest(unittest.TestCase):
    def test_ci_uses_the_local_node_free_msvc_setup(self) -> None:
        workflow = CI_BUILD.read_text(encoding="utf-8")
        self.assertNotIn(LEGACY_ACTION, workflow)
        self.assertGreaterEqual(workflow.count("uses: ./.github/actions/setup-msvc"), 2)

    def test_local_setup_exports_and_verifies_the_target_toolchain(self) -> None:
        action = SETUP_MSVC.read_text(encoding="utf-8")
        for marker in (
            "vswhere.exe",
            "Launch-VsDevShell.ps1",
            '$developerShellArch = if ($target -eq "x64") { "amd64" }',
            "GITHUB_ENV",
            "/Bv /c",
            "VSCMD_ARG_TGT_ARCH",
        ):
            self.assertIn(marker, action)

    def test_lint_rejects_the_legacy_node20_action(self) -> None:
        self.assertIn(LEGACY_ACTION, LINT.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
