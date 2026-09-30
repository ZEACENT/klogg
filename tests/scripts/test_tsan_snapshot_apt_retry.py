"""Exercise the snapshot transaction with shell-only, isolated command stubs."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
HELPER = ROOT / "docker" / "ubuntu22.04-tsan" / "apt_snapshot_retry.sh"
SH = shutil.which("sh")
DOCKERFILE = HELPER.parent / "Dockerfile"
POLICY = {
    "Acquire::Retries": "5",
    "Acquire::http::Timeout": "30",
    "Acquire::https::Timeout": "30",
    "APT::Update::Error-Mode": "any",
}

# No external commands are available to the helper except these three stubs.
# In particular, rm records arguments without touching any host APT files.
STUB = r'''#!/bin/sh
command=${0##*/}
{
    printf '%s\n' "$command"
    printf '%s\n' "$@"
    printf '%s\n' END
} >> "$FAKE_LOG"
case "$command" in
    rm) exit "${FAKE_RM_STATUS:-0}" ;;
    sleep) exit 0 ;;
esac
operation=
for argument do
    case "$argument" in update|install) operation=$argument ;; esac
done
case "$operation" in
    update) statuses=$FAKE_UPDATE_STATUSES ;;
    install) statuses=$FAKE_INSTALL_STATUSES ;;
    *) printf '%s\n' 'unexpected APT operation' >&2; exit 99 ;;
esac
count=0
if [ -f "$FAKE_STATE/$operation" ]; then
    IFS= read -r count < "$FAKE_STATE/$operation"
fi
count=$((count + 1))
printf '%s\n' "$count" > "$FAKE_STATE/$operation"
set -- $statuses
while [ "$count" -gt 1 ] && [ "$#" -gt 1 ]; do
    shift
    count=$((count - 1))
done
status=$1
if [ "$status" -ne 0 ]; then
    printf 'synthetic %s failure: %s\n' "$operation" "$status" >&2
fi
exit "$status"
'''


@unittest.skipUnless(SH, "a POSIX sh is required for snapshot APT helper tests")
class SnapshotAptRetryTest(unittest.TestCase):
    def run_helper(self, *packages, updates=(0,), installs=(0,), cleanup_status=0):
        self.assertTrue(HELPER.is_file(), "the shared snapshot retry helper is missing")
        with tempfile.TemporaryDirectory(prefix="snapshot apt ") as directory:
            root = pathlib.Path(directory)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            for command in ("apt-get", "sleep", "rm"):
                stub = fake_bin / command
                stub.write_text(STUB, encoding="utf-8")
                stub.chmod(0o755)
            log = root / "calls"
            env = os.environ.copy()
            env.update(
                PATH=str(fake_bin),
                FAKE_LOG=str(log),
                FAKE_STATE=str(root),
                FAKE_UPDATE_STATUSES=" ".join(map(str, updates)),
                FAKE_INSTALL_STATUSES=" ".join(map(str, installs)),
                FAKE_RM_STATUS=str(cleanup_status),
            )
            result = subprocess.run(
                [SH, str(HELPER), *packages],
                cwd=str(root),
                env=env,
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            calls = []
            if log.exists():
                for record in log.read_text().split("END\n"):
                    if record:
                        calls.append(record.splitlines())
            return result, calls

    def apt_operations(self, calls):
        return [
            next(arg for arg in call if arg in ("update", "install"))
            for call in calls if call[0] == "apt-get"
        ]

    def assert_attempt_cleanup(self, calls, attempts):
        cleanups = [call for call in calls if call[0] == "rm"]
        self.assertEqual(len(cleanups), attempts)
        for cleanup in cleanups:
            self.assertEqual(cleanup[1:3], ["-rf", "--"])
            self.assertTrue(cleanup[3:])
            for path in cleanup[3:]:
                self.assertTrue(path.startswith("/var/lib/apt/lists/"), path)
        for index, call in enumerate(calls):
            if call[0] == "apt-get" and "update" in call:
                self.assertGreater(index, 0)
                self.assertEqual(calls[index - 1][0], "rm")

    def test_two_transient_updates_retry_the_whole_transaction(self):
        result, calls = self.run_helper("ca-certificates", updates=(100, 100, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.apt_operations(calls), ["update"] * 3 + ["install"])
        self.assert_attempt_cleanup(calls, 3)
        self.assertEqual([call for call in calls if call[0] == "sleep"],
                         [["sleep", "15"], ["sleep", "30"]])
        for attempt in (1, 2, 3):
            self.assertIn("attempt {}/5".format(attempt), result.stderr)
        self.assertEqual(result.stderr.count("synthetic update failure: 100"), 2)
        self.assertEqual(result.stderr.count("failed with status 100"), 2)

    def test_install_failure_requires_a_fresh_update_before_retry(self):
        result, calls = self.run_helper("curl", installs=(42, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.apt_operations(calls),
                         ["update", "install", "update", "install"])
        self.assert_attempt_cleanup(calls, 2)
        self.assertIn("synthetic install failure: 42", result.stderr)
        self.assertIn("failed with status 42", result.stderr)

    def test_permanent_failures_stop_after_five_and_keep_the_final_status(self):
        for operation in ("update", "install"):
            with self.subTest(operation=operation):
                failures = {"updates" if operation == "update" else "installs":
                            (100, 42, 43, 44, 73)}
                result, calls = self.run_helper("curl", **failures)
                self.assertEqual(result.returncode, 73, result.stderr)
                expected = ["update"] if operation == "update" else ["update", "install"]
                self.assertEqual(self.apt_operations(calls), expected * 5)
                self.assert_attempt_cleanup(calls, 5)
                self.assertEqual([call for call in calls if call[0] == "sleep"],
                                 [["sleep", str(delay)] for delay in (15, 30, 45, 60)])
                self.assertIn("attempt 5/5 failed with status 73", result.stderr)
                self.assertNotIn("attempt 6/5", result.stderr)

    def test_every_apt_call_has_strict_bounded_acquisition_policy(self):
        result, calls = self.run_helper("ca-certificates", installs=(100, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        apt_calls = [call for call in calls if call[0] == "apt-get"]
        self.assertEqual(len(apt_calls), 4)
        for call in apt_calls:
            options = {}
            arguments = []
            tokens = iter(call[1:])
            for token in tokens:
                if token == "-o":
                    key, value = next(tokens).split("=", 1)
                    options[key] = value
                else:
                    arguments.append(token)
            self.assertEqual(options, POLICY)
            operation = arguments.pop(0)
            if operation == "update":
                self.assertEqual(arguments, ["-y"])
            else:
                self.assertCountEqual(arguments[:-1], ["--no-install-recommends", "-y"])
                self.assertEqual(arguments[-1], "ca-certificates")

    def test_first_attempt_success_never_sleeps_and_preserves_package_arguments(self):
        packages = ("libexample=1:2.3", "package with spaces", "literal*", "$(not-a-command)")
        result, calls = self.run_helper(*packages)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.apt_operations(calls), ["update", "install"])
        self.assertFalse(any(call[0] == "sleep" for call in calls))
        install = next(call for call in calls if "install" in call)
        self.assertEqual(install[-len(packages):], list(packages))
        self.assert_attempt_cleanup(calls, 1)

    def test_no_packages_means_update_only_including_retries(self):
        for updates in ((0,), (100, 0)):
            with self.subTest(updates=updates):
                result, calls = self.run_helper(updates=updates)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.apt_operations(calls), ["update"] * len(updates))
                self.assert_attempt_cleanup(calls, len(updates))
                self.assertEqual(sum(call[0] == "sleep" for call in calls), len(updates) - 1)

    def test_mixed_failures_share_one_budget_and_can_succeed_on_last_attempt(self):
        result, calls = self.run_helper(
            "curl", updates=(100, 0, 100, 0, 0), installs=(42, 43, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.apt_operations(calls), [
            "update", "update", "install", "update", "update", "install",
            "update", "install",
        ])
        self.assert_attempt_cleanup(calls, 5)
        self.assertEqual([call for call in calls if call[0] == "sleep"],
                         [["sleep", str(delay)] for delay in (15, 30, 45, 60)])

    def test_cleanup_failure_cannot_be_swallowed_or_reach_apt(self):
        result, calls = self.run_helper("curl", cleanup_status=17)
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual([call[0] for call in calls], ["rm"])


@unittest.skipUnless(SH, "a POSIX sh is required for snapshot bootstrap tests")
class SnapshotBootstrapTest(unittest.TestCase):
    def run_bootstrap(self, *, bootstrap_status=0, verified_status=0):
        # Execute the actual first RUN, but relocate every filesystem path and
        # replace package acquisition. The restricted rm stub rejects host paths.
        instructions = DOCKERFILE.read_text().replace("\\\n", " ").splitlines()
        run = next(line[4:] for line in instructions if line.startswith("RUN "))
        with tempfile.TemporaryDirectory(prefix="snapshot-bootstrap-") as directory:
            root = pathlib.Path(directory)
            apt = root / "etc" / "apt"
            (apt / "apt.conf.d").mkdir(parents=True)
            lists = root / "lists"
            lists.mkdir()
            (lists / "stale-index").touch()
            override = apt / "apt.conf.d" / "99snapshot-bootstrap"
            fake_bin = root / "bin"
            fake_bin.mkdir()
            helper = fake_bin / "apt_snapshot_retry.sh"
            helper.write_text(
                '#!/bin/sh\n'
                'tls=verified\n'
                '[ ! -e "$FAKE_OVERRIDE" ] || tls=bootstrap\n'
                'printf "%s|%s\\n" "$tls" "$*" >> "$FAKE_LOG"\n'
                'if [ "$#" -gt 0 ]; then exit "$FAKE_BOOTSTRAP_STATUS"; fi\n'
                'exit "$FAKE_VERIFIED_STATUS"\n'
            )
            helper.chmod(0o755)
            rm = fake_bin / "rm"
            rm.write_text(
                '#!/bin/sh\n'
                'for argument do\n'
                '  case "$argument" in\n'
                '    -*) ;;\n'
                '    "$FAKE_ROOT"/*) ;;\n'
                '    *) printf "unsafe rm path: %s\\n" "$argument" >&2; exit 99 ;;\n'
                '  esac\n'
                'done\n'
                'exec "$REAL_RM" "$@"\n'
            )
            rm.chmod(0o755)
            run = run.replace("/etc/apt/", str(apt) + "/")
            run = run.replace("/var/lib/apt/lists/", str(lists) + "/")
            run = run.replace("/usr/local/bin/apt_snapshot_retry.sh", str(helper))
            log = root / "calls"
            env = os.environ.copy()
            env.update(
                PATH=str(fake_bin),
                UBUNTU_SNAPSHOT="20260731T000000Z",
                FAKE_ROOT=str(root),
                REAL_RM=shutil.which("rm"),
                FAKE_OVERRIDE=str(override),
                FAKE_LOG=str(log),
                FAKE_BOOTSTRAP_STATUS=str(bootstrap_status),
                FAKE_VERIFIED_STATUS=str(verified_status),
            )
            result = subprocess.run(
                [SH, "-c", run], cwd=root, env=env, capture_output=True,
                text=True, check=False, timeout=10,
            )
            calls = log.read_text().splitlines() if log.exists() else []
            return result, calls, override.exists()

    def test_bootstrap_removes_tls_exception_before_verified_update(self):
        result, calls, override_exists = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, ["bootstrap|ca-certificates", "verified|"])
        self.assertFalse(override_exists)

    def test_bootstrap_failure_removes_tls_exception_and_preserves_status(self):
        result, calls, override_exists = self.run_bootstrap(bootstrap_status=73)
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertEqual(calls, ["bootstrap|ca-certificates"])
        self.assertFalse(override_exists)

    def test_verified_update_failure_stays_failed_without_tls_exception(self):
        result, calls, override_exists = self.run_bootstrap(verified_status=74)
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertEqual(calls, ["bootstrap|ca-certificates", "verified|"])
        self.assertFalse(override_exists)


if __name__ == "__main__":
    unittest.main()
