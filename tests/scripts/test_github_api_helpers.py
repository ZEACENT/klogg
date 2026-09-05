from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import textwrap
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

    def run_continuous_selection(self, mode: str) -> tuple[subprocess.CompletedProcess, str]:
        workflow = CONTINUOUS_WORKFLOW.read_text(encoding="utf-8")
        step = workflow.split(
            "      - name: Verify dispatched CI run and current master tip", 1
        )[1].split("\n\n  publish:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fake_gh = root / "gh"
            state = root / "state"
            output = root / "github-output"
            fake_gh.write_text(
                """#!/usr/bin/env bash
set -euo pipefail
if [ "$FAKE_GH_MODE" = api-error ]; then
  printf 'gh: simulated API failure\\n' >&2
  exit 1
fi
if [ "$FAKE_GH_MODE" = transient ] && [ ! -e "$FAKE_GH_TRANSIENT_STATE" ]; then
  : > "$FAKE_GH_TRANSIENT_STATE"
  printf 'gh: simulated transient API failure\\n' >&2
  exit 1
fi
shift
endpoint=""
jq_filter=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --jq) jq_filter="$2"; shift 2 ;;
    --paginate|--slurp) shift ;;
    -*) shift ;;
    *)
      if [ -z "$endpoint" ]; then endpoint="$1"; fi
      shift
      ;;
  esac
done
case "$endpoint" in
  */branches/master)
    printf '%s\\n' "$EXPECTED_SHA"
    ;;
  */jobs?*)
    if [ "$FAKE_GH_MODE" = gate-failure ]; then
      printf 'completed\\tfailure\\n'
    elif [ "$FAKE_GH_MODE" = retry-gate ] && [[ "$endpoint" == */attempts/2/* ]]; then
      printf '\\n'
    else
      printf 'completed\\tsuccess\\n'
    fi
    ;;
  */actions/runs/*)
    case "$jq_filter" in
      *'.head_repository.full_name'*'@tsv'*)
        if [ "$FAKE_GH_MODE" = identity-failure ]; then
          printf '.github/workflows/other.yml\\tZEACENT/klogg\\tmaster\\tpush\\t%s\\n' "$EXPECTED_SHA"
        else
          printf '.github/workflows/ci-build.yml\\tZEACENT/klogg\\tmaster\\tpush\\t%s\\n' "$EXPECTED_SHA"
        fi
        ;;
      .path)
        if [ "$FAKE_GH_MODE" = identity-failure ]; then
          printf '.github/workflows/other.yml\\n'
        else
          printf '.github/workflows/ci-build.yml\\n'
        fi
        ;;
      '.head_repository.full_name // empty') printf 'ZEACENT/klogg\\n' ;;
      '.head_branch // empty') printf 'master\\n' ;;
      .event) printf 'push\\n' ;;
      .head_sha) printf '%s\\n' "$EXPECTED_SHA" ;;
      .run_attempt)
        if [ "$FAKE_GH_MODE" = retry-gate ]; then printf '2\\n'; else printf '1\\n'; fi
        ;;
      '[.status, (.conclusion // "")] | @tsv')
        count="$(cat "$FAKE_GH_STATE" 2>/dev/null || printf 0)"
        case "$FAKE_GH_MODE" in
          failure) printf 'completed\\tfailure\\n' ;;
          transition)
            count=$((count + 1))
            printf '%s\\n' "$count" > "$FAKE_GH_STATE"
            if [ "$count" -eq 1 ]; then printf 'in_progress\\t\\n'; else printf 'completed\\tsuccess\\n'; fi
            ;;
          timeout) printf 'in_progress\\t\\n' ;;
          *) printf 'completed\\tsuccess\\n' ;;
        esac
        ;;
      .status)
        count="$(cat "$FAKE_GH_STATE" 2>/dev/null || printf 0)"
        case "$FAKE_GH_MODE" in
          transition)
            count=$((count + 1))
            printf '%s\\n' "$count" > "$FAKE_GH_STATE"
            if [ "$count" -eq 1 ]; then printf 'in_progress\\n'; else printf 'completed\\n'; fi
            ;;
          timeout) printf 'in_progress\\n' ;;
          *) printf 'completed\\n' ;;
        esac
        ;;
      '.conclusion // ""')
        case "$FAKE_GH_MODE" in
          failure) printf 'failure\\n' ;;
          transition)
            count="$(cat "$FAKE_GH_STATE" 2>/dev/null || printf 0)"
            if [ "$count" -le 1 ]; then printf '\\n'; else printf 'success\\n'; fi
            ;;
          timeout) printf '\\n' ;;
          *) printf 'success\\n' ;;
        esac
        ;;
      *) printf 'unexpected jq filter: %s\\n' "$jq_filter" >&2; exit 2 ;;
    esac
    ;;
  *) printf 'unexpected endpoint: %s\\n' "$endpoint" >&2; exit 2 ;;
esac
""",
                encoding="utf-8",
            )
            fake_gh.chmod(0o755)
            env = os.environ.copy()
            env.update(
                {
                    "PATH": f"{root}{os.pathsep}{env['PATH']}",
                    "FAKE_GH_MODE": mode,
                    "FAKE_GH_STATE": str(state),
                    "FAKE_GH_TRANSIENT_STATE": str(root / "transient-state"),
                    "EXPECTED_SHA": "a" * 40,
                    "GITHUB_REPOSITORY": "ZEACENT/klogg",
                    "GITHUB_OUTPUT": str(output),
                    "KLOGG_REQUESTED_CI_RUN_ID": "12345",
                    "KLOGG_REQUESTED_CI_RUN_SHA": "a" * 40,
                    "KLOGG_DISPATCH_SHA": "a" * 40,
                    "KLOGG_CI_POLL_ATTEMPTS": "2",
                    "KLOGG_CI_POLL_INTERVAL_SECONDS": "0",
                    "KLOGG_GH_API_RETRY_ATTEMPTS": "2",
                    "KLOGG_GH_API_RETRY_DELAY_SECONDS": "0",
                }
            )
            result = subprocess.run(
                ["bash", "-c", script],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            emitted = output.read_text(encoding="utf-8") if output.exists() else ""
            return result, emitted

    def test_continuous_selection_waits_for_success_and_fails_closed(self):
        for mode in ("success", "transition", "retry-gate", "transient"):
            with self.subTest(mode=mode):
                result, emitted = self.run_continuous_selection(mode)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("ci-run-id=12345", emitted)
                self.assertIn(f"ci-run-sha={'a' * 40}", emitted)
                self.assertIn("should-publish=true", emitted)
        for mode in ("failure", "timeout", "gate-failure", "identity-failure", "api-error"):
            with self.subTest(mode=mode):
                result, emitted = self.run_continuous_selection(mode)
                self.assertNotEqual(result.returncode, 0, mode)
                self.assertNotIn("should-publish=true", emitted)

    def test_release_cleanup_uses_the_optional_scalar_lookup(self):
        continuous = workflow_section(
            CONTINUOUS_WORKFLOW,
            "      - name: Clean stale continuous candidate draft",
            "      - name: Create continuous candidate draft",
        )
        stable = workflow_section(
            STABLE_WORKFLOW,
            "      - name: Clean stale stable draft",
            "      - name: Recheck Continuous snapshot before stable draft creation",
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
