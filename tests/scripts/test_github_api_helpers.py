from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
HELPERS = ROOT / "scripts" / "github_api_helpers.sh"
CONTINUOUS_WORKFLOW = ROOT / ".github" / "workflows" / "ci-continuous.yml"
STABLE_WORKFLOW = ROOT / ".github" / "workflows" / "ci-release.yml"


def workflow_section(path: pathlib.Path, start: str, end: str) -> str:
    text = path.read_text(encoding="utf-8")
    return text.split(start, 1)[1].split(end, 1)[0]


@unittest.skipUnless(shutil.which("bash"), "bash is required for GitHub API helper tests")
class GitHubApiHelpersTest(unittest.TestCase):
    def run_lookup(self, mode: str, initial: str = "sentinel") -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as directory:
            bin_dir = pathlib.Path(directory)
            fake_gh = bin_dir / "gh"
            fake_gh.write_text(
                """#!/usr/bin/env bash
case "$FAKE_GH_MODE" in
  success)
    printf '12345\\n'
    exit 0
    ;;
  missing)
    printf '{"message":"Not Found","status":404}\\n'
    printf 'gh: Not Found (HTTP 404)\\n' >&2
    exit 1
    ;;
  error)
    printf '{"message":"Server Error","status":500}\\n'
    printf 'gh: Server Error (HTTP 500)\\n' >&2
    exit 1
    ;;
  *)
    exit 2
    ;;
esac
""",
                encoding="utf-8",
            )
            fake_gh.chmod(0o755)
            env = os.environ.copy()
            env["FAKE_GH_MODE"] = mode
            env["PATH"] = f"{bin_dir}{os.pathsep}{env['PATH']}"
            return subprocess.run(
                [
                    "bash",
                    "-c",
                    (
                        f'source "{HELPERS}"\n'
                        f'value="{initial}"\n'
                        "set +e\n"
                        "gh_api_optional_scalar value /repos/example/releases/tags/test .id\n"
                        "status=$?\n"
                        "printf 'status=%s\\nvalue=<%s>\\n' \"$status\" \"$value\"\n"
                    ),
                ],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
            )

    def test_success_assigns_the_requested_scalar(self):
        result = self.run_lookup("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "status=0\nvalue=<12345>\n")
        self.assertEqual(result.stderr, "")

    def test_not_found_discards_error_json_and_clears_destination(self):
        result = self.run_lookup("missing")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "status=1\nvalue=<>\n")
        self.assertEqual(result.stderr, "")

    def test_non_404_failure_clears_destination_and_reports_error(self):
        result = self.run_lookup("error")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "status=2\nvalue=<>\n")
        self.assertIn("gh: Server Error (HTTP 500)", result.stderr)

    def test_release_cleanup_uses_the_optional_scalar_lookup(self):
        continuous = workflow_section(
            CONTINUOUS_WORKFLOW,
            "      - name: Clean stale continuous candidate draft",
            "      - name: Create continuous candidate draft",
        )
        stable = workflow_section(
            STABLE_WORKFLOW,
            "    - name: Clean stale stable draft",
            "    - name: Recheck master tip before stable draft creation",
        )
        for name, cleanup in (("continuous", continuous), ("stable", stable)):
            with self.subTest(workflow=name):
                self.assertIn("source scripts/github_api_helpers.sh", cleanup)
                self.assertIn("gh_api_optional_scalar release_id", cleanup)
                self.assertNotIn('release_id="$(gh api ', cleanup)

        self.assertNotIn('elif [ -n "$release_id" ] ||', continuous)
        self.assertIn(
            'if [ "$tag" = "$final_tag" ] && [ -z "$release_id" ]',
            stable,
        )

    def test_all_optional_release_scalar_lookups_use_the_shared_helper(self):
        unsafe_lookup = re.compile(
            r'if\s+[a-zA-Z_][a-zA-Z0-9_]*="\$\(gh api [^\n]+2>"\$[^"\n]+"\)"'
        )
        for path in (CONTINUOUS_WORKFLOW, STABLE_WORKFLOW):
            with self.subTest(workflow=path.name):
                workflow = path.read_text(encoding="utf-8")
                self.assertNotRegex(workflow, unsafe_lookup)

        continuous = CONTINUOUS_WORKFLOW.read_text(encoding="utf-8")
        for destination in ("live_id", "release_id", "published_tag"):
            self.assertIn(f"gh_api_optional_scalar {destination}", continuous)
        self.assertGreaterEqual(
            continuous.count("source scripts/github_api_helpers.sh"),
            4,
        )


if __name__ == "__main__":
    unittest.main()
