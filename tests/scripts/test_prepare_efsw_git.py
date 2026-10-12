"""Behavioral tests for the private efsw Git process owner."""
import contextlib
import importlib.util
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "prepare_efsw_git.py"


def load_driver():
    spec = importlib.util.spec_from_file_location("prepare_efsw_git", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# Each child owns only its connection to this test. EOF is proof that the
# ordinary descendant was terminated, not a timing or PID-liveness guess.
DESCENDANT = """
import socket, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
s.sendall(b'R')
s.recv(1)
"""


class PrepareEfswGitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = load_driver()

    def cli_command(self, arguments, environment=None):
        original = os.environ if environment is None else environment
        startup = ["--startup-lc-ctype-state", "present" if "LC_CTYPE" in original else "absent"]
        if "LC_CTYPE" in original:
            startup.append("--startup-lc-ctype=" + original["LC_CTYPE"])
        return [sys.executable, "-I", "-S", str(SCRIPT)] + startup + arguments

    def git_observer_environment(self, original=None):
        environment = dict(os.environ if original is None else original)
        # Test-only filesystem ownership: these override even explicit Git -C.
        # The production inherit operation still preserves all four verbatim.
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY"):
            environment.pop(name, None)
        return environment

    @contextlib.contextmanager
    def owned_git_context(self, environment=None):
        # Shell aliases perform repository discovery. Their observer context
        # must not depend on an unavailable worktree target in the caller cwd.
        with tempfile.TemporaryDirectory() as temporary:
            subprocess.run(["git", "-C", temporary, "init", "-q"], check=True,
                           env=self.git_observer_environment(environment),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            yield pathlib.Path(temporary)

    def run_python(self, code, *args, **kwargs):
        return self.driver._run(
            [sys.executable, "-c", code] + list(args),
            timeout=kwargs.pop("timeout", 5), env=os.environ.copy(), **kwargs
        )

    def test_normal_output_nonzero_and_large_binary_streams(self):
        for exitcode in (0, 17):
            with self.subTest(exitcode=exitcode):
                result = self.run_python(
                    "import os,sys; os.write(1,b'x'*262144+b'\\xff'); "
                    "os.write(2,b'original stderr\\x00'); sys.exit(int(sys.argv[1]))",
                    str(exitcode),
                )
                self.assertEqual(result.kind, "ok" if exitcode == 0 else "git")
                self.assertEqual(result.returncode, exitcode)
                self.assertEqual(result.stdout, b"x" * 262144 + b"\xff")
                self.assertEqual(result.stderr, b"original stderr\x00")

    def test_stdin_is_nul(self):
        result = self.run_python("import sys; print(repr(sys.stdin.buffer.read()))")
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout, b"b''\n")

    def test_command_environment_is_not_changed_by_intermediate_interpreter(self):
        environment = os.environ.copy()
        environment.pop("KLOGG_ANCHOR_MUTATED", None)
        anchor = "import os; os.environ['KLOGG_ANCHOR_MUTATED'] = 'wrong'\n" + self.driver._POSIX_ANCHOR
        with mock.patch.object(self.driver, "_POSIX_ANCHOR", anchor):
            result = self.driver._run(
                [sys.executable, "-I", "-S", "-c", "import os; print(repr(os.environ.get('KLOGG_ANCHOR_MUTATED')))"],
                timeout=5, env=environment,
            )
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout, b"None\n")

    def test_c_locale_environment_is_delivered_without_python_coercion(self):
        environment = os.environ.copy()
        environment["LANG"] = "C"
        for key in ("LC_ALL", "LC_CTYPE", "PYTHONCOERCECLOCALE", "PYTHONUTF8"):
            environment.pop(key, None)
        # Observe the command environment with a native observer, not another
        # Python interpreter that could itself perform PEP 538 locale coercion.
        if os.name == "nt":
            command = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c",
                       "if defined LC_CTYPE (echo present) else (echo absent)"]
        else:
            command = ["/bin/sh", "-c", 'if [ "${LC_CTYPE+x}" = x ]; then printf present; else printf absent; fi']
        result = self.driver._run(command, timeout=5, env=environment)
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout.strip(), b"absent")

    def test_owner_bootstrap_cannot_import_modules_from_command_cwd(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name in ("json", "subprocess"):
                pathlib.Path(temporary, name + ".py").write_text("raise RuntimeError('cwd module must not be imported')\n")
            result = self.run_python("print('trusted')", cwd=temporary)
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout, b"trusted\n")

    def test_child_receives_exact_environment_arguments_and_working_directory(self):
        environment = os.environ.copy()
        environment.pop("KLOGG_OWNER_ABSENT", None)
        environment.update(KLOGG_OWNER_EMPTY="", KLOGG_OWNER_VALUE="unchanged")
        original = environment.copy()
        result = self.driver._run(
            [sys.executable, "-c", "import json,os,sys; print(json.dumps([dict(os.environ),sys.argv[1:],os.getcwd()]))",
             "space argument", "quote\"and\\slash", ""],
            timeout=5, env=environment, cwd=str(ROOT / "tests" / "scripts"),
        )
        self.assertEqual(result.kind, "ok", result.diagnostic)
        received, arguments, cwd = json.loads(result.stdout)
        self.assertEqual(received, original)
        self.assertEqual(environment, original)
        self.assertEqual(arguments, ["space argument", "quote\"and\\slash", ""])
        self.assertEqual(pathlib.Path(cwd).resolve(), (ROOT / "tests" / "scripts").resolve())

    def test_environment_is_a_copy_and_preserves_absent_empty_and_value(self):
        keys = ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT")
        for value in (None, "", "original"):
            original = {"PATH": os.environ.get("PATH", ""), "UNRELATED": "retained"}
            if value is not None:
                original.update({key: value for key in keys})
            before = original.copy()
            for operation in ("inherit", "config", "clone"):
                result = self.driver._environment(operation, "global", "ceiling", original)
                self.assertEqual(original, before)
                self.assertIsNot(result, original)
                self.assertEqual(result["UNRELATED"], "retained")
                removed = keys[:3] if operation == "config" else keys[3:] if operation == "clone" else ()
                for key in keys:
                    if key in removed or value is None:
                        self.assertNotIn(key, result)
                    else:
                        self.assertEqual(result[key], value)
                if operation == "config":
                    self.assertEqual(result["GIT_CEILING_DIRECTORIES"], "ceiling")
                if operation == "clone":
                    self.assertEqual(result["GIT_CONFIG_GLOBAL"], "global")

    def descendant_case(self, leader_exits):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = str(listener.getsockname()[1])
        # A private control connection holds the leader independently of stdin
        # (which the driver deliberately binds to NUL).
        control = socket.socket()
        control.bind(("127.0.0.1", 0))
        control.listen(1)
        control.settimeout(10)
        code = (
            "import socket,subprocess,sys; "
            "c=socket.create_connection(('127.0.0.1',int(sys.argv[3]))); "
            "subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]]); "
            + "c.recv(1)"
        )
        box = []
        errors = []

        def run():
            try:
                box.append(self.run_python(code, DESCENDANT, port, str(control.getsockname()[1]), timeout=5))
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=run, daemon=True)
        peer = leader = None
        try:
            worker.start()
            leader, _ = control.accept()
            leader.settimeout(10)
            peer, _ = listener.accept()
            peer.settimeout(10)
            self.assertEqual(peer.recv(1), b"R")
            if leader_exits:
                leader.sendall(b"x")
            worker.join(10)
            self.assertFalse(worker.is_alive(), "driver cleanup did not finish")
            self.assertEqual(errors, [])
            self.assertEqual(box[0].kind, "ok" if leader_exits else "timeout", box[0].diagnostic)
            self.assertEqual(peer.recv(1), b"", "ordinary child survived owned cleanup")
            self.assertEqual(leader.recv(1), b"", "leader survived owned cleanup")
        finally:
            # Release only this test's IPC peers on RED or unexpected failure.
            for connection in (leader, peer):
                if connection is not None:
                    try:
                        connection.sendall(b"release")
                    except OSError:
                        pass
                    connection.close()
            listener.close()
            control.close()
            worker.join(10)

    def test_timeout_terminates_live_leader_and_ordinary_child(self):
        self.descendant_case(False)

    def test_leader_exit_terminates_inherited_output_holder(self):
        self.descendant_case(True)

    def test_windows_fault_models_fail_closed_and_preserve_owned_handles(self):
        driver = self.driver
        for stage in ("create", "create-finalize", "verify", "resume", "wait", "terminate", "quiescence", "close"):
            with self.subTest(stage=stage):
                calls = []
                closed = []

                class API:
                    def create_job(self):
                        calls.append("job")
                        return "job", "port"

                    def create_suspended(self, owner, argv, env, cwd, job):
                        calls.append("create")
                        if stage == "create":
                            raise driver._Failure("containment", "original atomic creation failure")
                        owner.process, owner.thread = "process", "thread"
                        if stage == "create-finalize":
                            raise driver._Failure("cleanup", "post-create native cleanup failure")

                    def verify_job(self, process, job):
                        calls.append("verify")
                        if stage == "verify":
                            raise driver._Failure("containment", "original assignment verification failure")

                    def resume(self, thread):
                        calls.append("resume")
                        if stage == "resume":
                            raise driver._Failure("containment", "original resume failure")

                    def wait(self, process, deadline):
                        calls.append("wait")
                        if stage == "wait" and calls.count("wait") == 1:
                            raise driver._Failure("containment", "original wait failure")
                        return 17

                    def terminate(self, job):
                        calls.append("terminate")
                        if stage == "terminate":
                            raise OSError("termination cleanup failure")

                    def quiescence(self, port, deadline):
                        calls.append("quiescence")
                        if stage == "quiescence":
                            raise OSError("quiescence cleanup failure")

                    def close(self, handle):
                        closed.append(handle)
                        if stage == "close" and handle == "thread":
                            raise OSError("close cleanup failure")

                result = driver._run(["fixture"], timeout=5, env={},
                                     _owner_factory=lambda: driver._WindowsOwner(API))
                self.assertEqual(result.kind, "containment" if stage in ("create", "verify", "resume", "wait") else "cleanup", result.diagnostic)
                self.assertEqual(closed, ["job", "port"] if stage == "create" else ["thread", "process", "job", "port"])
                self.assertIn("terminate", calls)
                if stage in ("create", "verify"):
                    self.assertNotIn("resume", calls)
                if stage != "create":
                    self.assertIn("quiescence", calls)
                if stage in ("create", "verify", "resume", "wait"):
                    self.assertIn("original", result.diagnostic)
                else:
                    if stage != "create-finalize":
                        self.assertIn("Git exited with status 17", result.diagnostic)
                    self.assertIn("cleanup failure", result.diagnostic)

    def test_windows_completion_order_is_assignment_resume_then_owned_quiescence(self):
        calls = []
        driver = self.driver

        class API:
            def create_job(self):
                calls.append("job")
                return "job", "port"

            def create_suspended(self, owner, argv, env, cwd, job):
                calls.append("atomic-create-suspended")
                owner.process, owner.thread = "process", "thread"

            def verify_job(self, process, job):
                calls.append("verify-assignment")

            def resume(self, thread):
                calls.append("resume")

            def wait(self, process, deadline):
                calls.append("wait")
                return 0

            def terminate(self, job):
                calls.append("terminate-job")

            def quiescence(self, port, deadline):
                calls.append("active-process-zero")

            def close(self, handle):
                calls.append("close-" + handle)

        result = driver._run(["fixture"], timeout=5, env={},
                             _owner_factory=lambda: driver._WindowsOwner(API))
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(calls, ["job", "atomic-create-suspended", "verify-assignment", "resume", "wait",
                                 "terminate-job", "active-process-zero", "wait", "close-thread", "close-process",
                                 "close-job", "close-port"])

    def test_launch_error_is_typed(self):
        result = self.driver._run([str(ROOT / "does-not-exist")], timeout=5, env=os.environ.copy())
        self.assertEqual(result.kind, "spawn", result.diagnostic)
        self.assertIn("does-not-exist", result.diagnostic)

    def test_cli_preserves_git_nonzero_and_does_not_collide_with_driver_codes(self):
        result = subprocess.run(
            self.cli_command(["--timeout", "5", "--operation", "inherit", "--", "not-a-git-command"]),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"not-a-git-command", result.stderr)
        self.assertIn(b"Git exited with status", result.stderr)

    def test_posix_owner_refuses_an_auto_reaping_signal_disposition(self):
        driver = self.driver
        owner = driver._PosixOwner()
        with mock.patch.object(driver.signal, "SIGCHLD", 20, create=True):
            with mock.patch.object(driver.signal, "getsignal", return_value=signal.SIG_IGN):
                with mock.patch.object(driver.subprocess, "Popen", side_effect=AssertionError("must refuse before spawn")):
                    with self.assertRaises(driver._Failure) as captured:
                        owner.start(["fixture"], {}, None)
        self.assertEqual(captured.exception.kind, "containment")

    def test_cli_establishes_child_reaping_even_if_caller_ignored_sigchld(self):
        environment = self.git_observer_environment()
        options = {}
        if os.name == "posix":
            options["preexec_fn"] = lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN)
        with self.owned_git_context() as repository:
            result = subprocess.run(
                self.cli_command(["--timeout", "5", "--operation", "inherit", "--", "-C", str(repository),
                                  "-c", "alias.fail=!exit 17", "fail"], environment),
                env=environment, capture_output=True, timeout=10, **options
            )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b"Git exited with status 17", result.stderr)

    def test_cli_requires_explicit_pre_interpreter_locale_state(self):
        result = subprocess.run(
            [sys.executable, "-I", "-S", str(SCRIPT), "--timeout", "5", "--operation", "inherit", "--", "--version"],
            capture_output=True, timeout=10,
        )
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn(b"--startup-lc-ctype-state", result.stderr)

    def test_cli_restores_absent_empty_and_value_before_python_startup(self):
        alias = '!printf "%s" "${LC_CTYPE+present:}${LC_CTYPE}"'
        for value in (None, "", "C", "space value", "-dash=equals;semicolon"):
            with self.subTest(value=value), self.owned_git_context() as repository:
                environment = self.git_observer_environment()
                environment["LANG"] = "C"
                for key in ("LC_ALL", "LC_CTYPE", "PYTHONCOERCECLOCALE", "PYTHONUTF8"):
                    environment.pop(key, None)
                if value is not None:
                    environment["LC_CTYPE"] = value
                original = environment.copy()
                result = subprocess.run(
                    self.cli_command(["--timeout", "5", "--operation", "inherit", "--", "-C", str(repository),
                                      "-c", "alias.snapshot=" + alias, "snapshot"], environment),
                    env=environment, capture_output=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, b"" if value is None else ("present:" + value).encode("utf-8"))
                self.assertEqual(environment, original)

    def test_owned_git_context_cannot_write_to_inherited_external_roots(self):
        keys = ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY")
        parent_environment = os.environ.copy()
        parent_cwd = pathlib.Path.cwd()
        for key in keys:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temporary:
                source = dict(parent_environment)
                for other in keys:
                    source.pop(other, None)
                external = pathlib.Path(temporary) / "external-root"
                source[key] = str(external)
                original = source.copy()
                with self.owned_git_context(source) as repository:
                    self.assertTrue((repository / ".git").is_dir())
                    observer_environment = self.git_observer_environment(source)
                    before = observer_environment.copy()
                    arguments = self.cli_command([
                        "--timeout", "5", "--operation", "inherit", "--", "-C", str(repository),
                        "-c", "alias.context=!git rev-parse --show-toplevel", "context",
                    ], observer_environment)
                    result = subprocess.run(arguments, env=observer_environment, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(pathlib.Path(os.fsdecode(result.stdout.strip())).resolve(), repository.resolve())
                    self.assertEqual(observer_environment, before)
                self.assertFalse(external.exists(), "fixture wrote outside its owned Git context")
                self.assertEqual(source, original)
        self.assertEqual(dict(os.environ), parent_environment)
        self.assertEqual(pathlib.Path.cwd(), parent_cwd)

    def test_owned_git_alias_context_does_not_move_worker_from_unavailable_caller_repo(self):
        environment = self.git_observer_environment()
        original_environment = environment.copy()
        original_cwd = pathlib.Path.cwd()
        with tempfile.TemporaryDirectory() as temporary, self.owned_git_context() as repository:
            caller = pathlib.Path(temporary)
            (caller / ".git").write_text("gitdir: " + str(caller / "unavailable-checkout.git") + "\n")
            broken = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=caller,
                                    env=environment, capture_output=True, timeout=10)
            self.assertEqual(broken.returncode, 128, broken.stderr)
            self.assertIn(b"unavailable-checkout.git", broken.stderr)
            arguments = self.cli_command([
                "--timeout", "5", "--operation", "inherit", "--", "-C", str(repository),
                "-c", "alias.context=!git rev-parse --show-toplevel", "context",
            ], environment)
            # Execute the actual driver main in this isolated worker so both
            # pre/post worker cwd can be observed, not merely the outer parent.
            launcher = (
                "import json,os,runpy,sys; before=os.getcwd(); "
                "driver=runpy.run_path(sys.argv[1]); result=driver['main'](sys.argv[2:]); "
                "sys.stderr.write(json.dumps([before,os.getcwd()])); sys.exit(result)"
            )
            result = subprocess.run([sys.executable, "-I", "-S", "-c", launcher] + arguments[3:],
                                    cwd=caller, env=environment, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(pathlib.Path(os.fsdecode(result.stdout.strip())).resolve(), repository.resolve())
            before, after = json.loads(result.stderr)
            self.assertEqual(pathlib.Path(before).resolve(), caller.resolve())
            self.assertEqual(pathlib.Path(after).resolve(), caller.resolve())
        self.assertEqual(environment, original_environment)
        self.assertEqual(pathlib.Path.cwd(), original_cwd)

    def test_cli_git_success_interpreter_and_usage(self):
        result = subprocess.run(
            self.cli_command(["--timeout", "5", "--operation", "inherit", "--", "--version"]),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b"git version "))
        for arguments, expected in ((["--check-interpreter"], 0), (["--timeout", "nan", "--operation", "inherit", "--", "--version"], 3), ([], 3)):
            with self.subTest(arguments=arguments):
                check = subprocess.run([sys.executable, str(SCRIPT)] + arguments, capture_output=True, timeout=10)
                self.assertEqual(check.returncode, expected, check.stderr)

    def test_bounded_cleanup_does_not_close_a_pipe_with_a_blocked_reader(self):
        driver = self.driver
        closed = []

        class Pipe:
            def close(self):
                closed.append(True)
                raise AssertionError("closing a CRT FD held by a blocked reader can hang")

        class Owner:
            stderr = None
            stdout = Pipe()

            def start(self, argv, env, cwd):
                pass

            def wait(self, deadline):
                return 0

            def cleanup(self, deadline):
                raise OSError("owned termination failed")

        class BlockedReader:
            def __init__(self, **kwargs):
                pass

            def start(self):
                pass

            def join(self, timeout):
                pass

            def is_alive(self):
                return True

        with mock.patch.object(driver.threading, "Thread", BlockedReader):
            result = driver._run(["fixture"], timeout=5, env={}, _owner_factory=Owner)
        self.assertEqual(result.kind, "cleanup", result.diagnostic)
        self.assertIn("owned termination failed", result.diagnostic)
        self.assertEqual(closed, [])

    def test_completed_operation_is_not_retroactively_timed_out_by_cleanup(self):
        driver = self.driver
        now = [0.0]

        class Owner:
            stdout = stderr = None

            def start(self, argv, env, cwd):
                pass

            def wait(self, deadline):
                now[0] = 1.0
                return 0

            def cleanup(self, deadline):
                # Cleanup is valid within its separate safety deadline (6).
                now[0] = 5.5

        with mock.patch.object(driver.time, "monotonic", side_effect=lambda: now[0]):
            result = driver._run(["fixture"], timeout=5, env={}, _owner_factory=Owner)
        self.assertEqual(result.kind, "ok", result.diagnostic)

    def test_os_wait_failure_is_containment_not_launch_failure(self):
        driver = self.driver

        class Owner:
            stdout = stderr = None

            def start(self, argv, env, cwd):
                pass

            def wait(self, deadline):
                raise OSError("original wait syscall failure")

            def cleanup(self, deadline):
                pass

        result = driver._run(["fixture"], timeout=5, env={}, _owner_factory=Owner)
        self.assertEqual(result.kind, "containment", result.diagnostic)
        self.assertIn("original wait syscall failure", result.diagnostic)

    def test_cleanup_failure_keeps_original_containment_diagnostic(self):
        driver = self.driver

        class Owner:
            stdout = stderr = None

            def start(self, argv, env, cwd):
                raise driver._Failure("containment", "original resume failure")

            def wait(self, deadline):
                self.fail("must not wait after failed start")

            def cleanup(self, deadline):
                raise OSError("secondary cleanup failure")

        result = driver._run(["fixture"], timeout=5, env={}, _owner_factory=Owner)
        self.assertEqual(result.kind, "cleanup", result.diagnostic)
        self.assertIn("original resume failure", result.diagnostic)
        self.assertIn("secondary cleanup failure", result.diagnostic)

    def test_fault_models_keep_primary_error_and_close_owner(self):
        driver = self.driver
        for stage, kind in (("start", "containment"), ("wait", "containment"), ("cleanup", "cleanup")):
            calls = []

            class Owner:
                stdout = stderr = None

                def start(self, argv, env, cwd):
                    calls.append("start")
                    if stage == "start":
                        raise driver._Failure("containment", "original assignment failure")

                def wait(self, deadline):
                    calls.append("wait")
                    if stage == "wait":
                        raise driver._Failure("containment", "original wait failure")
                    return 17

                def cleanup(self, deadline):
                    calls.append("cleanup")
                    if stage == "cleanup":
                        raise OSError("cleanup failed")

            result = driver._run(["fixture"], timeout=5, env={}, _owner_factory=Owner)
            self.assertEqual(result.kind, kind, result.diagnostic)
            self.assertIn("cleanup", calls)
            if stage == "cleanup":
                self.assertEqual(result.returncode, 17)
                self.assertIn("Git exited with status 17", result.diagnostic)
                self.assertIn("cleanup failed", result.diagnostic)
            else:
                self.assertIn("original", result.diagnostic)


if __name__ == "__main__":
    unittest.main()
