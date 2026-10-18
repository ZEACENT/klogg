"""Optional diagnostics sample evidence, never own or alter build execution."""
import contextlib
import importlib
import io
import json
import os
from pathlib import Path
import signal
import select
import subprocess
import time
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))


class ExecutionDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.module = importlib.import_module("ci_execution_diagnostics")
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_activation_requires_explicit_macos_thread_scope(self):
        for platform, sanitizer, mode in (("linux", "thread", "macos-tsan"),
                                          ("darwin", "address", "macos-tsan"),
                                          ("darwin", "thread", None)):
            with self.subTest(platform=platform, sanitizer=sanitizer, mode=mode), \
                    mock.patch.object(self.module.sys, "platform", platform), \
                    mock.patch.dict(os.environ, {"KLOGG_SANITIZER": sanitizer}, clear=True), \
                    mock.patch.object(self.module.subprocess, "Popen") as popen:
                self.assertIsNone(self.module.start(self.root, mode))
                popen.assert_not_called()

    def test_descendants_exclude_sampler_subtree_and_other_jobs(self):
        text = "10 1 0.0 100 S 0:01.00 /observer\n11 10 40.0 200 R 0:02.25 /bin/ccache\n12 11 60.0 300 U 1:00.00 /toolchain/clang++\n"
        text += "20 10 10.0 50 S 0:00.50 /sampler\n21 20 20.0 75 R 0:00.75 /bin/ps\n99 1 99.0 999 R 0:99.00 /unrelated\n"
        value = self.module.descendants(text, 10, 20, max_rows=1)
        self.assertEqual(value["observed_count"], 2)
        self.assertEqual(value["rss_kib_sum"], 500)
        self.assertEqual(value["ps_cpu_percent_sum"], 100)
        self.assertTrue(value["rows_truncated"])
        self.assertEqual(value["processes"][0]["pid"], 12)
        self.assertEqual(value["processes"][0]["cpu_seconds"], 60)
        self.assertNotIn(99, [p["pid"] for p in value["processes"]])

    def test_process_roles_discard_unknown_names_paths_and_never_request_argv(self):
        text = ("11 10 20.0 300 R 0:01.00 /private/SECRET/toolchain/clang\n"
                "12 10 30.0 200 R 0:02.00 /private/SECRET/ccache\n"
                "13 10 40.0 100 S 0:03.00 /private/SECRET/ninja\n"
                "14 10 50.0 50 S 0:04.00 /private/SECRET/custom executable\n")
        value = self.module.descendants(text, 10, 20)
        self.assertEqual([row["role"] for row in value["processes"]],
                         ["compiler", "cache", "build-tool", "other"])
        self.assertNotIn("SECRET", json.dumps(value))
        self.assertNotIn("private", json.dumps(value))
        self.assertNotIn("custom", json.dumps(value))
        # The production format requests executable names, never command arguments.
        self.assertEqual(self.module.PS_COMMAND,
                         ["/bin/ps", "-axo", "pid=,ppid=,%cpu=,rss=,state=,time=,comm="])

    def test_parsers_allow_only_numeric_facts_and_known_keys(self):
        text = "direct_cache_hit 17\ncache_miss 4\nstats_zeroed_timestamp 20\nPRIVATE_TOKEN secret\n"
        self.assertEqual(self.module.cache_counters(text),
                         {"direct_cache_hit": 17, "cache_miss": 4, "stats_zeroed_timestamp": 20})
        self.assertEqual(self.module.tool_version("ccache", "ccache version 4.14\nPRIVATE_TOKEN=secret"), [4, 14])
        self.assertIsNone(self.module.tool_version("ccache", "PRIVATE_TOKEN=123456"))
        self.assertIsNone(self.module.positive_integer("-j4 PRIVATE_TOKEN=secret"))
        self.assertEqual(self.module.positive_integer("4"), 4)
        self.assertEqual(self.module.swap_facts("total = 2.00G used = 3.00M free = 1.00G (encrypted)"),
                         {"total_bytes": 2147483648, "used_bytes": 3145728, "free_bytes": 1073741824})
        facts = self.module.vm_facts("Mach Virtual Memory Statistics: (page size of 4096 bytes)\nPages free: 100.\nPageouts: 2.\nPRIVATE_TOKEN: 123.\n")
        self.assertEqual(facts, {"page_size_bytes": 4096, "pages_free": 100, "pageouts": 2})

    def test_effective_ccache_config_is_allowlisted_and_redacts_custom_policies(self):
        text = ("(environment) compiler_check = content\n(default) compression = true\n"
                "(environment) compression_level = 6\n(environment) max_size = 250M\n"
                "(/private/secret.conf) remote_storage = redis://user:PRIVATE_TOKEN@host\n"
                "(environment) cache_dir = /private/secret\n")
        facts = self.module.cache_configuration(text)
        self.assertEqual(facts, {"compiler_check": "content", "compression": True,
                                 "compression_level": 6, "max_size_bytes": 250000000,
                                 "remote_storage_enabled": True})
        self.assertNotIn("PRIVATE", json.dumps(facts))
        self.assertNotIn("secret", json.dumps(facts))
        self.assertEqual(self.module.cache_configuration("(env) compiler_check = echo PRIVATE_TOKEN=secret\n"),
                         {"compiler_check": "custom"})
        self.assertEqual(self.module.cache_configuration("(env) max_size = 2Gi\n(default) remote_storage = \n"),
                         {"max_size_bytes": 2147483648, "remote_storage_enabled": False})

    def test_journal_is_partial_bounded_and_log_backed(self):
        with contextlib.redirect_stdout(io.StringIO()) as console:
            writer = self.module.Journal(self.root, max_bytes=900)
            try:
                for index in range(20):
                    writer.emit({"kind": "sample", "value": index})
            finally:
                writer.close()
        self.assertLessEqual(sum(p.stat().st_size for p in self.root.glob("*.jsonl*")), 1800)
        rows = [json.loads(line) for line in (self.root / "ci-build-diagnostics.jsonl").read_text().splitlines()]
        self.assertEqual(rows[-1]["value"], 19)
        self.assertTrue(all(r["scope"] == "sampled-partial" for r in rows))
        self.assertTrue(all("build_wall_ms" not in r for r in rows))
        logged = [json.loads(line.split("=", 1)[1]) for line in console.getvalue().splitlines()]
        self.assertEqual(len(logged), 20)
        self.assertEqual(logged[-1]["value"], 19)

    @unittest.skipUnless(os.name == "posix", "POSIX no-follow checkpoint")
    def test_checkpoint_is_bounded_raw_data_and_does_not_touch_ninja_state(self):
        data = b"# ninja log v5\n" + b"0\t100\t0\tobject.o\t123\n" * 20
        path = self.root / ".ninja_log"
        path.write_bytes(data)
        result = self.module.checkpoint(self.root, max_bytes=100)
        self.assertTrue(result["truncated"])
        copied = (self.root / "ci-build-diagnostics.ninja-log.txt").read_bytes()
        self.assertLessEqual(len(copied), 100)
        self.assertTrue(data.endswith(copied))
        self.assertEqual(path.read_bytes(), data)
        self.assertEqual(result["latest_completed_ms"], 100)
        self.assertEqual(self.module.checkpoint(self.root / "missing")["status"], "unavailable")

    @unittest.skipUnless(os.name == "posix", "POSIX no-follow and process lifecycle")
    def test_symlink_checkpoint_input_is_not_read(self):
        secret = self.root / "secret"
        secret.write_text("PRIVATE_TOKEN=secret")
        (self.root / ".ninja_log").symlink_to(secret)
        self.assertEqual(self.module.checkpoint(self.root)["status"], "unavailable")
        self.assertFalse((self.root / "ci-build-diagnostics.ninja-log.txt").exists())

    def test_probe_timeout_and_errors_are_bounded_and_private(self):
        process = mock.Mock()
        process.stdout.fileno.return_value = 100
        process.poll.return_value = None
        ticks = iter((0, 0, 2))
        with mock.patch.object(self.module.subprocess, "Popen", return_value=process), \
                mock.patch.object(self.module.select, "select", return_value=([], [], [])), \
                mock.patch.object(self.module.time, "monotonic", side_effect=lambda: next(ticks)):
            probe = self.module.Probe(lambda: False, timeout=1)
            self.assertIsNone(probe.run(["ccache", "--print-stats"]))
        process.kill.assert_called_once_with()
        process.wait.assert_called_once()
        self.assertEqual(probe.failures, 1)
        with mock.patch.object(self.module.subprocess, "Popen", side_effect=OSError("PRIVATE_TOKEN=secret")):
            self.assertIsNone(probe.run(["ccache", "--print-stats"]))
        self.assertEqual(probe.failures, 2)

    def test_probe_eof_exit_transition_keeps_successful_output(self):
        for terminal in (0, 23):
            with self.subTest(terminal=terminal):
                process = mock.Mock()
                # EOF can become visible before waitpid observes process exit.
                # A second poll in the same branch must not turn success into failure.
                process.poll.side_effect = (None, terminal, terminal)
                with mock.patch.object(self.module.subprocess, "Popen", return_value=process), \
                        mock.patch.object(self.module.select, "select", return_value=([process.stdout], [], [])), \
                        mock.patch.object(self.module.os, "read", side_effect=(b"1.12.1\n", b"", b"")):
                    probe = self.module.Probe(lambda: False)
                    value = probe.run(["ninja", "--version"])
                self.assertEqual(value, "1.12.1\n" if terminal == 0 else None)
                self.assertEqual(probe.failures, int(terminal != 0))
                process.kill.assert_not_called()
                process.wait.assert_called_once()

    def test_oversize_probe_output_is_discarded_and_reaped(self):
        process = mock.Mock()
        process.poll.return_value = None
        with mock.patch.object(self.module.subprocess, "Popen", return_value=process), \
                mock.patch.object(self.module.select, "select", return_value=([process.stdout], [], [])), \
                mock.patch.object(self.module.os, "read", return_value=b"x" * (self.module.PROBE_BYTES + 1)):
            probe = self.module.Probe(lambda: False)
            self.assertIsNone(probe.run(["ccache", "--print-stats"]))
        process.kill.assert_called_once_with()
        process.wait.assert_called_once()
        self.assertEqual(probe.failures, 1)

    @unittest.skipUnless(os.name == "posix", "POSIX memory and no-follow checkpoint")
    def test_sampler_probe_failure_still_publishes_partial_checkpoint(self):
        (self.root / ".ninja_log").write_text("# ninja log v5\n0\t1\t0\tx.o\t1\n")
        probe = mock.Mock(failures=4)
        probe.run.return_value = None
        data = self.module.sample(self.root, 1, probe)
        self.assertIsNone(data["processes"])
        self.assertIsNone(data["ccache_counters"])
        self.assertEqual(data["probe_failures"], 4)
        self.assertEqual(data["ninja"]["status"], "available")
        self.assertNotIn("build_wall_ms", data)

    def test_initial_facts_do_not_disclose_environment_or_tool_output(self):
        outputs = {"/usr/sbin/sysctl": "8\n4\n34359738368\n", "cmake": "cmake version 3.31.6\nSECRET=private",
                   "ninja": "1.12.1\nSECRET=private", "ccache": "ccache version 4.14\nSECRET=private"}
        probe = mock.Mock(failures=0)
        probe.run.side_effect = lambda command: outputs[command[0]]
        value = self.module.initial_facts(self.root, probe)
        self.assertEqual(value["hardware"], {"logical_cpus": 8, "physical_cpus": 4, "ram_bytes": 34359738368})
        self.assertEqual(value["tool_versions"]["cmake"], [3, 31, 6])
        self.assertNotIn("SECRET", json.dumps(value))
        self.assertNotIn("private", json.dumps(value))

    def test_unreadable_generated_compiler_metadata_does_not_stop_sampling(self):
        compiler = self.root / "CMakeFiles/3.31/CMakeCXXCompiler.cmake"
        compiler.parent.mkdir(parents=True)
        compiler.write_text("metadata")
        probe = mock.Mock(failures=0)
        probe.run.return_value = None
        with mock.patch.object(Path, "read_text", side_effect=OSError("PRIVATE_TOKEN=secret")):
            facts = self.module.initial_facts(self.root, probe)
        self.assertEqual(facts["compiler_versions"], {})
        self.assertNotIn("secret", json.dumps(facts))

    def test_probe_stop_reaps_only_its_own_child(self):
        process = mock.Mock()
        process.poll.return_value = None
        stopped = iter((False, False, True))
        with mock.patch.object(self.module.subprocess, "Popen", return_value=process), \
                mock.patch.object(self.module.select, "select", return_value=([], [], [])), \
                mock.patch.object(self.module.os, "killpg", create=True) as killpg:
            probe = self.module.Probe(lambda: next(stopped))
            self.assertIsNone(probe.run(["ccache", "--print-stats"]))
        process.kill.assert_called_once_with()
        process.wait.assert_called_once()
        killpg.assert_not_called()

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_session_stop_cannot_signal_a_build_sibling(self):
        build = subprocess.Popen([sys.executable, "-I", "-c", "import sys; sys.stdin.buffer.read()"],
                                 stdin=subprocess.PIPE)
        sampler = subprocess.Popen([sys.executable, "-I", "-B", str(ROOT / "scripts/ci_execution_diagnostics.py"),
                                    "--watch", str(self.root), str(os.getpid())],
                                   stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, start_new_session=True)
        try:
            self.module.Session(sampler).stop()
            self.assertIsNotNone(sampler.poll())
            self.assertIsNone(build.poll())
        finally:
            if sampler.poll() is None:
                os.killpg(sampler.pid, signal.SIGKILL)
                sampler.wait(timeout=5)
            build.communicate(timeout=5)
        self.assertEqual(build.returncode, 0)

    @unittest.skipUnless(os.name == "posix", "POSIX diagnostic signal cleanup")
    def test_sampler_signal_interrupts_interval_without_waiting_for_parent_pipe(self):
        process = subprocess.Popen([sys.executable, "-I", "-B", str(ROOT / "scripts/ci_execution_diagnostics.py"),
                                    "--watch", str(self.root), str(os.getpid())],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, start_new_session=True)
        try:
            # Wait for semantic sample completion, not a guessed startup delay.
            pending = b""
            sampled = False
            deadline = time.monotonic() + 15
            while not sampled:
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, "sampler did not publish its first sample")
                self.assertTrue(select.select([process.stdout], [], [], remaining)[0])
                chunk = os.read(process.stdout.fileno(), 65536)
                self.assertTrue(chunk, "sampler exited before first sample")
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    record = json.loads(line.split(b"=", 1)[1])
                    sampled = sampled or record["kind"] == "sample"
            process.send_signal(signal.SIGTERM)
            # Outer deadlock bound; the parent control pipe remains open.
            self.assertEqual(process.wait(timeout=2), 0)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()

    @unittest.skipUnless(os.name == "posix", "POSIX owner-death lifecycle")
    def test_actual_owner_death_closes_pipe_and_sampler_exits(self):
        code = ("import os,subprocess,sys; "
                "p=subprocess.Popen([sys.executable,'-I','-B',sys.argv[1],'--watch',sys.argv[2],str(os.getpid())],"
                "stdin=subprocess.PIPE,start_new_session=True); "
                "print('SAMPLER_PID='+str(p.pid),flush=True); os._exit(0)")
        owner = subprocess.Popen([sys.executable, "-I", "-c", code,
                                  str(ROOT / "scripts/ci_execution_diagnostics.py"), str(self.root)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            stdout, stderr = owner.communicate(timeout=5)
            self.assertEqual(owner.returncode, 0, stderr.decode())
            records = [json.loads(line.split(b"=", 1)[1]) for line in stdout.splitlines()
                       if line.startswith(self.module.PREFIX.encode())]
            self.assertEqual(records[-1]["kind"], "stop")
            self.assertFalse(list(self.root.glob("ci-build-metrics*")))
        finally:
            if owner.poll() is None:
                os.killpg(owner.pid, signal.SIGKILL)
                owner.wait(timeout=5)

    @unittest.skipUnless(os.name == "posix", "POSIX sampler control pipe")
    def test_closed_parent_pipe_stops_sampler_without_fabricating_build_report(self):
        process = subprocess.Popen([sys.executable, "-I", "-B", str(ROOT / "scripts/ci_execution_diagnostics.py"),
                                    "--watch", str(self.root), str(os.getpid())],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        try:
            stdout, stderr = process.communicate(input=b"", timeout=5)
            self.assertEqual(process.returncode, 0, stderr.decode())
            rows = [json.loads(line.split(b"=", 1)[1]) for line in stdout.splitlines()]
            self.assertTrue(rows)
            self.assertTrue(all(row["scope"] == "sampled-partial" for row in rows))
            self.assertFalse(list(self.root.glob("ci-build-metrics*")))
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
