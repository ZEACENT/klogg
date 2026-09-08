from __future__ import annotations

import contextlib
import http.server
import json
import os
import threading
import urllib.parse
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
# Model actual gh's local flag validation rather than accepting impossible calls.
if [[ " $* " == *" --slurp "* && " $* " == *" --jq "* ]]; then
  printf 'gh: the `--slurp` option is not supported with `--jq` or `--template`\\n' >&2
  exit 1
fi
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
      printf '[{"jobs":[{"name":"ci-gate","status":"completed","conclusion":"failure"}]}]\\n'
    elif [ "$FAKE_GH_MODE" = retry-gate ] && [[ "$endpoint" == */attempts/2/* ]]; then
      printf '[{"jobs":[]}]\\n'
    else
      printf '[{"jobs":[{"name":"ci-gate","status":"completed","conclusion":"success"}]}]\\n'
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
                # The select job intentionally buffers retry stdout; it is not
                # an optional release scalar lookup and is tested with real gh.
                publication = workflow.split("\n  publish:", 1)[-1]
                self.assertNotRegex(publication, unsafe_lookup)

        continuous = CONTINUOUS_WORKFLOW.read_text(encoding="utf-8")
        for destination in ("live_id", "release_id", "published_tag"):
            self.assertIn(f"gh_api_optional_scalar {destination}", continuous)
        self.assertGreaterEqual(
            continuous.count("source scripts/github_api_helpers.sh"),
            4,
        )


class RealGitHubCliTest(unittest.TestCase):
    """Run the installed CLI, never authenticated configuration or remote APIs."""

    @classmethod
    def setUpClass(cls):
        # This module is in required run_ci_quality discovery. Missing tools are
        # failures, not silently skipped coverage of the publisher's CLI contract.
        for tool in ("gh", "jq", "bash"):
            if shutil.which(tool) is None:
                raise AssertionError(f"{tool} is required for the real GitHub CLI regression")

    @contextlib.contextmanager
    def fixture(self, mode="success"):
        requests = []
        sha = "a" * 40

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                requests.append(self.path)
                url = urllib.parse.urlsplit(self.path)
                status = 200
                next_page = False
                page_two = urllib.parse.parse_qs(url.query).get("page") == ["2"]
                first_page_two = sum("page=2" in request for request in requests) == 1
                if (mode == "api-error" or (mode == "transient" and len(requests) == 1)
                        or (mode == "page-error" and page_two)
                        or (mode == "page-transient" and page_two and first_page_two)):
                    status, body = 500, {"message": "simulated API failure"}
                elif url.path.endswith("/jobs"):
                    page = urllib.parse.parse_qs(url.query).get("page", ["1"])[0]
                    attempt = url.path.split("/attempts/")[1].split("/")[0]
                    gate = {"name": "ci-gate", "status": "completed", "conclusion": "success"}
                    if mode == "gate-failure" or (mode == "wrong-attempt" and attempt == "2"):
                        gate["conclusion"] = "failure"
                    if mode == "missing" or (mode == "retry-gate" and attempt == "2"):
                        body = {"jobs": []}
                    elif page == "1":
                        build = {"name": "build", "status": "completed", "conclusion": "success"}
                        body = {"jobs": [gate if mode == "ambiguous" else build]}
                        next_page = True
                    else:
                        body = {"jobs": [gate]}
                elif url.path.endswith("/branches/master"):
                    body = {"commit": {"sha": "b" * 40 if mode == "stale" else sha}}
                else:
                    body = {
                        "path": ".github/workflows/ci-build.yml",
                        "head_repository": {"full_name": "example/klogg"},
                        "head_branch": "master", "event": "push", "head_sha": sha,
                        "run_attempt": 2 if mode in ("retry-gate", "wrong-attempt") else 1,
                        "status": "completed", "conclusion": "failure" if mode == "run-failure" else "success",
                    }
                    if mode == "identity-failure":
                        body["head_sha"] = "b" * 40
                payload = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                if next_page:
                    host, port = self.server.server_address
                    self.send_header("Link", f'<http://{host}:{port}{url.path}?per_page=100&page=2>; rel="next"')
                self.end_headers()
                self.wfile.write(payload)

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
            thread.start()
            try:
                base_url = f"http://127.0.0.1:{server.server_port}"
                real_gh = shutil.which("gh")
                shim = root / "gh"
                shim.write_text(
                    '#!/usr/bin/env bash\nset -euo pipefail\nargs=()\n'
                    'for arg in "$@"; do\n'
                    '  case "$arg" in /repos/*) arg="${LOCAL_API}${arg}" ;; esac\n'
                    '  args+=("$arg")\ndone\nexec "$REAL_GH" "${args[@]}"\n'
                )
                shim.chmod(0o755)
                # Allowlist only. In particular no real GH/GITHUB tokens, hosts,
                # proxies, debug logging, or user configuration can escape here.
                env = {
                    "PATH": f"{root}{os.pathsep}{os.environ['PATH']}",
                    "HOME": str(root), "GH_CONFIG_DIR": str(root / "config"),
                    "GH_TOKEN": "dummy-local-fixture-token", "GH_PROMPT_DISABLED": "1",
                    "REAL_GH": real_gh, "LOCAL_API": base_url,
                    "NO_PROXY": "127.0.0.1", "LC_ALL": "C",
                    "GITHUB_REPOSITORY": "example/klogg", "GITHUB_OUTPUT": str(root / "output"),
                    "KLOGG_REQUESTED_CI_RUN_ID": "12345", "KLOGG_REQUESTED_CI_RUN_SHA": sha,
                    "KLOGG_DISPATCH_SHA": sha, "KLOGG_CI_POLL_ATTEMPTS": "2",
                    "KLOGG_CI_POLL_INTERVAL_SECONDS": "0", "KLOGG_GH_API_RETRY_ATTEMPTS": "2",
                    "KLOGG_GH_API_RETRY_DELAY_SECONDS": "0",
                }
                yield env, requests
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
                self.assertFalse(thread.is_alive(), "loopback API server did not stop")

    def run_script(self, env, script):
        return subprocess.run(["bash", "-c", script], cwd=ROOT, env=env,
                              capture_output=True, text=True, timeout=20, check=False)

    def selection_script(self):
        return textwrap.dedent(workflow_section(
            CONTINUOUS_WORKFLOW, "        run: |\n", "\n\n  publish:"
        ))

    def test_actual_cli_rejects_slurp_with_jq_or_template_before_network(self):
        with self.fixture() as (env, requests):
            for option in ("--jq '.[]'", "-q '.[]'", "--template '{{.}}'", "-t '{{.}}'"):
                with self.subTest(option=option):
                    result = self.run_script(env, f'gh api --paginate --slurp /repos/example/klogg/jobs {option}')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("the `--slurp` option is not supported with `--jq` or `--template`", result.stderr)
            self.assertEqual(requests, [])

    def test_corrected_cli_aggregates_all_pages_before_raw_jq(self):
        with self.fixture() as (env, requests):
            result = self.run_script(env, '''set -euo pipefail
pages="$(gh api --paginate --slurp /repos/example/klogg/actions/runs/12345/attempts/1/jobs?per_page=100)"
jq -r '[.[].jobs[] | select(.name == "ci-gate")][0] | [.status, .conclusion] | @tsv' <<< "$pages"
''')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "completed\tsuccess\n")
            self.assertEqual(len(requests), 2)
            self.assertIn("page=2", requests[-1])

    def test_real_workflow_selects_later_page_gate_and_retry_attempt(self):
        for mode in ("success", "retry-gate", "transient", "page-transient"):
            with self.subTest(mode=mode), self.fixture(mode) as (env, requests):
                result = self.run_script(env, self.selection_script())
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("should-publish=true", pathlib.Path(env["GITHUB_OUTPUT"]).read_text())
                self.assertTrue(any("page=2" in request for request in requests))
                if mode == "transient":
                    self.assertIn("HTTP 500", result.stderr)

    def test_real_workflow_fails_closed(self):
        for mode in ("missing", "gate-failure", "ambiguous", "wrong-attempt", "api-error", "page-error", "stale", "identity-failure", "run-failure"):
            with self.subTest(mode=mode), self.fixture(mode) as (env, requests):
                result = self.run_script(env, self.selection_script())
                output = pathlib.Path(env["GITHUB_OUTPUT"])
                self.assertNotIn("should-publish=true", output.read_text() if output.exists() else "")
                if mode == "stale":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("should-publish=false", output.read_text())
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                if mode == "api-error":
                    self.assertEqual(len(requests), 2)
                    self.assertIn("failed after 2 attempts", result.stderr)
                if mode == "wrong-attempt":
                    self.assertFalse(any("/attempts/1/" in request for request in requests))

    def test_retry_discards_failed_output_and_does_not_retry_cli_usage_errors(self):
        wrapper = self.selection_script().split('run_api=', 1)[0]
        with self.fixture("transient") as (env, requests):
            result = self.run_script(env, wrapper + '\ngh_api_retry /repos/example/klogg/actions/runs/12345\n')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["head_sha"], "a" * 40)
            self.assertEqual(len(requests), 2)
        with self.fixture() as (env, requests):
            result = self.run_script(env, wrapper + '\ngh_api_retry --paginate --slurp /repos/example/klogg/jobs --jq ".[]"\n')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stderr.count("the `--slurp` option is not supported with `--jq` or `--template`"), 1)
            self.assertNotIn("GitHub API request failed", result.stderr)
            self.assertEqual(requests, [])


if __name__ == "__main__":
    unittest.main()
