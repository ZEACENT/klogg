"""Behavioral tests for the private efsw Git process owner."""
import contextlib
import importlib.util
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
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


# Private readiness identifies the actual fixture process. On Windows the
# FILETIME binds its reported PID to a pinned, read-only native process handle.
_FIXTURE_IDENTITY = r'''
import os, struct
def identity(tag):
    stamp = 0
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        native = ctypes.WinDLL('kernel32', use_last_error=True)
        native.GetCurrentProcess.argtypes = []
        native.GetCurrentProcess.restype = wintypes.HANDLE
        native.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        native.GetProcessTimes.restype = wintypes.BOOL
        created, exited, kernel, user = [wintypes.FILETIME() for _ in range(4)]
        if not native.GetProcessTimes(native.GetCurrentProcess(), ctypes.byref(created),
                                      ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        stamp = (created.dwHighDateTime << 32) | created.dwLowDateTime
    return tag + struct.pack('!IIQ', os.getpid(), os.getppid(), stamp)
'''
DESCENDANT = _FIXTURE_IDENTITY + """
import socket, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
s.sendall(identity(b'R'))
s.recv(1)
"""


class _PinnedWindowsFixtureProcess:
    """Read-only identity and exit observations for these fixture children."""
    def __init__(self, pid, expected_stamp, _native=None):
        import ctypes
        from ctypes import wintypes
        self.c = ctypes
        self.w = wintypes
        self.native = _native if _native is not None else ctypes.WinDLL('kernel32', use_last_error=True)
        native = self.native
        native.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        native.OpenProcess.restype = wintypes.HANDLE
        native.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        native.GetProcessTimes.restype = wintypes.BOOL
        native.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        native.WaitForSingleObject.restype = wintypes.DWORD
        native.CloseHandle.argtypes = [wintypes.HANDLE]
        native.CloseHandle.restype = wintypes.BOOL
        # SYNCHRONIZE | QUERY_LIMITED_INFORMATION, non-inheritable. No mutation,
        # job assignment, process termination, or broad process enumeration.
        self.handle = native.OpenProcess(0x100000 | 0x1000, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            created, exited, kernel, user = [wintypes.FILETIME() for _ in range(4)]
            if not native.GetProcessTimes(self.handle, ctypes.byref(created), ctypes.byref(exited),
                                          ctypes.byref(kernel), ctypes.byref(user)):
                raise ctypes.WinError(ctypes.get_last_error())
            stamp = (created.dwHighDateTime << 32) | created.dwLowDateTime
            if stamp != expected_stamp:
                raise AssertionError('Fixture process creation identity changed before handle capture')
            if native.WaitForSingleObject(self.handle, 0) != 0x102:
                raise AssertionError('Fixture process was not alive while its handle was pinned')
        except BaseException:
            self.close()
            raise

    def wait_for_exit(self):
        status = self.native.WaitForSingleObject(self.handle, 10000)
        if status == 0xFFFFFFFF:
            raise self.c.WinError(self.c.get_last_error())
        if status != 0:
            raise AssertionError('Owned fixture process is still alive after driver cleanup')
        return True

    def close(self):
        if self.handle:
            if not self.native.CloseHandle(self.handle):
                raise self.c.WinError(self.c.get_last_error())
            self.handle = None


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

    def test_binary_output_preserves_lf_crlf_nul_and_non_ascii_bytes(self):
        stdout = b"LF\nCRLF\r\nCR\rNUL\x00BYTE\xff"
        stderr = b"stderr\r\n\x00\n"
        result = self.run_python(
            "import os,sys; os.write(1,bytes.fromhex(sys.argv[1])); os.write(2,bytes.fromhex(sys.argv[2]))",
            stdout.hex(), stderr.hex(),
        )
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout, stdout)
        self.assertEqual(result.stderr, stderr)

    def test_stdin_is_nul(self):
        result = self.run_python("import os,sys; os.write(1,repr(sys.stdin.buffer.read()).encode('ascii')+b'\\n')")
        self.assertEqual(result.kind, "ok", result.diagnostic)
        self.assertEqual(result.stdout, b"b''\n")

    def test_command_environment_is_not_changed_by_intermediate_interpreter(self):
        environment = os.environ.copy()
        environment.pop("KLOGG_ANCHOR_MUTATED", None)
        anchor = "import os; os.environ['KLOGG_ANCHOR_MUTATED'] = 'wrong'\n" + self.driver._POSIX_ANCHOR
        with mock.patch.object(self.driver, "_POSIX_ANCHOR", anchor):
            result = self.driver._run(
                [sys.executable, "-I", "-S", "-c", "import os; os.write(1,repr(os.environ.get('KLOGG_ANCHOR_MUTATED')).encode('ascii')+b'\\n')"],
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
            result = self.run_python("import os; os.write(1,b'trusted\\n')", cwd=temporary)
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

    def assert_owned_terminal_socket(self, connection, outcome, expected_kind, *,
                                     cleanup_complete, identity_ready, process_exit_observed,
                                     windows=None):
        is_windows = os.name == "nt" if windows is None else windows
        self.assertTrue(cleanup_complete, "socket state precedes completed owned cleanup")
        self.assertTrue(identity_ready, "owned fixture identity was not armed")
        self.assertIn(expected_kind, ("ok", "timeout"))
        self.assertEqual(outcome.kind, expected_kind, outcome.diagnostic)
        if is_windows:
            self.assertTrue(process_exit_observed, "owned native process handles have not signaled")
        try:
            disconnected = connection.recv(1)
        except ConnectionResetError as error:
            if not is_windows or getattr(error, "winerror", None) != 10054:
                raise
            return
        self.assertEqual(disconnected, b"", "owned fixture process survived cleanup")

    def test_windows_reset_requires_completed_owned_cleanup_and_native_exit(self):
        reset = ConnectionResetError("controlled abrupt disconnect")
        reset.winerror = 10054
        connection = mock.Mock()
        connection.recv.side_effect = reset
        self.assert_owned_terminal_socket(
            connection, self.driver._Outcome("ok"), "ok", cleanup_complete=True,
            identity_ready=True, process_exit_observed=True, windows=True,
        )
        connection.recv.assert_called_once_with(1)

    def test_terminal_socket_rejects_other_resets_and_timeouts(self):
        def reset(code):
            error = ConnectionResetError("controlled reset")
            error.winerror = code
            return error

        for windows, error in ((True, reset(10053)), (False, reset(10054)),
                               (True, ConnectionResetError("no Windows code")),
                               (True, socket.timeout("controlled deadlock bound")),
                               (True, OSError("unrelated socket error"))):
            with self.subTest(windows=windows, error=type(error).__name__):
                connection = mock.Mock()
                connection.recv.side_effect = error
                with self.assertRaises(type(error)):
                    self.assert_owned_terminal_socket(
                        connection, self.driver._Outcome("ok"), "ok", cleanup_complete=True,
                        identity_ready=True, process_exit_observed=True, windows=windows,
                    )

    def test_terminal_socket_rejects_unproven_cleanup_identity_status_and_exit(self):
        invalid = (
            {"cleanup_complete": False}, {"identity_ready": False},
            {"process_exit_observed": False}, {"kind": "git"}, {"kind": "spawn"},
            {"kind": "containment"}, {"kind": "cleanup"}, {"expected_kind": "git", "kind": "git"},
        )
        for changes in invalid:
            with self.subTest(changes=changes):
                connection = mock.Mock()
                connection.recv.return_value = b""
                options = {"cleanup_complete": True, "identity_ready": True,
                           "process_exit_observed": True, "windows": True}
                options.update({key: value for key, value in changes.items()
                                if key not in ("kind", "expected_kind")})
                with self.assertRaises(AssertionError):
                    self.assert_owned_terminal_socket(
                        connection, self.driver._Outcome(changes.get("kind", "ok")),
                        changes.get("expected_kind", "ok"), **options
                    )
                connection.recv.assert_not_called()

    def test_native_socket_eof_is_not_cleanup_evidence_before_worker_completion(self):
        reader, writer = socket.socketpair()
        try:
            reader.settimeout(5)
            writer.sendall(b"R")
            self.assertEqual(reader.recv(1), b"R")
            writer.shutdown(socket.SHUT_WR)
            self.assertEqual(reader.recv(1), b"")
            with self.assertRaises(AssertionError):
                self.assert_owned_terminal_socket(
                    reader, self.driver._Outcome("ok"), "ok", cleanup_complete=False,
                    identity_ready=True, process_exit_observed=True, windows=(os.name == "nt"),
                )
        finally:
            reader.close()
            writer.close()

    def test_native_socket_eof_after_owned_cleanup_is_terminal(self):
        reader, writer = socket.socketpair()
        try:
            reader.settimeout(5)
            writer.sendall(b"R")
            self.assertEqual(reader.recv(1), b"R")
            writer.shutdown(socket.SHUT_WR)
            self.assert_owned_terminal_socket(
                reader, self.driver._Outcome("timeout"), "timeout", cleanup_complete=True,
                identity_ready=True, process_exit_observed=True, windows=(os.name == "nt"),
            )
        finally:
            reader.close()
            writer.close()

    def test_windows_process_observer_rejects_alive_descendant_despite_reset_and_ok(self):
        native = mock.Mock()
        native.OpenProcess.return_value = 555
        native.CloseHandle.return_value = True
        native.WaitForSingleObject.side_effect = [0x102, 0x102]
        stamp = (11 << 32) | 7

        def process_times(handle, created, *others):
            created._obj.dwLowDateTime = 7
            created._obj.dwHighDateTime = 11
            return True

        native.GetProcessTimes.side_effect = process_times
        observer = _PinnedWindowsFixtureProcess(12345, stamp, _native=native)
        try:
            exited = False
            with self.assertRaises(AssertionError):
                exited = observer.wait_for_exit()
            reset = ConnectionResetError("controlled Windows reset from a live descendant")
            reset.winerror = 10054
            connection = mock.Mock()
            connection.recv.side_effect = reset
            with self.assertRaises(AssertionError):
                self.assert_owned_terminal_socket(
                    connection, self.driver._Outcome("ok"), "ok", cleanup_complete=True,
                    identity_ready=True, process_exit_observed=exited, windows=True,
                )
            connection.recv.assert_not_called()
        finally:
            observer.close()
        native.OpenProcess.assert_called_once_with(0x100000 | 0x1000, False, 12345)
        self.assertEqual(native.WaitForSingleObject.call_args_list, [mock.call(555, 0), mock.call(555, 10000)])
        native.CloseHandle.assert_called_once_with(555)

    def test_windows_process_observer_closes_handle_on_changed_or_already_dead_identity(self):
        stamp = (11 << 32) | 7
        for expected_stamp, initial_state in ((stamp + 1, 0x102), (stamp, 0)):
            with self.subTest(expected_stamp=expected_stamp, initial_state=initial_state):
                native = mock.Mock()
                native.OpenProcess.return_value = 555
                native.CloseHandle.return_value = True
                native.WaitForSingleObject.return_value = initial_state

                def process_times(handle, created, *others):
                    created._obj.dwLowDateTime = 7
                    created._obj.dwHighDateTime = 11
                    return True

                native.GetProcessTimes.side_effect = process_times
                with self.assertRaises(AssertionError):
                    _PinnedWindowsFixtureProcess(12345, expected_stamp, _native=native)
                native.CloseHandle.assert_called_once_with(555)

    def read_fixture_identity(self, connection, tag):
        packet = bytearray()
        while len(packet) < 17:
            chunk = connection.recv(17 - len(packet))
            self.assertNotEqual(chunk, b"", "fixture closed before complete identity readiness")
            packet.extend(chunk)
        self.assertEqual(packet[:1], tag)
        pid, parent_pid, creation_stamp = struct.unpack("!IIQ", packet[1:])
        self.assertGreater(pid, 0)
        self.assertGreater(parent_pid, 0)
        self.assertNotEqual(pid, os.getpid())
        if os.name == "nt":
            self.assertGreater(creation_stamp, 0)
        return pid, parent_pid, creation_stamp

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
        code = _FIXTURE_IDENTITY + (
            "\nimport socket,subprocess,sys\n"
            "c=socket.create_connection(('127.0.0.1',int(sys.argv[3])))\n"
            "c.sendall(identity(b'L'))\n"
            "subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]])\n"
            "c.recv(1)\n"
        )
        box = []
        errors = []
        capture_ack = threading.Event()
        wait_entered = threading.Event()
        deadlines = []
        driver = self.driver

        class CaptureGatedWindowsOwner(driver._WindowsOwner):
            def wait(self, deadline):
                # Keep native start/cleanup intact and the original deadline.
                # Cleanup cannot race PID/stamp capture behind this bounded ACK.
                deadlines.append(deadline)
                wait_entered.set()
                if not capture_ack.wait(10):
                    raise driver._Failure("containment", "fixture identity capture ACK was not released")
                return super().wait(deadline)

        def run():
            try:
                owner_factory = CaptureGatedWindowsOwner if os.name == "nt" else None
                box.append(self.run_python(code, DESCENDANT, port, str(control.getsockname()[1]),
                                           timeout=5, _owner_factory=owner_factory))
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
            leader_pid, _, leader_stamp = self.read_fixture_identity(leader, b"L")
            child_pid, child_parent, child_stamp = self.read_fixture_identity(peer, b"R")
            self.assertNotEqual(child_pid, leader_pid)
            self.assertEqual(child_parent, leader_pid)
            with contextlib.ExitStack() as captured:
                observers = []
                if os.name == "nt":
                    self.assertTrue(wait_entered.wait(10), "native owner did not enter the capture barrier")
                    for pid, stamp in ((leader_pid, leader_stamp), (child_pid, child_stamp)):
                        observer = _PinnedWindowsFixtureProcess(pid, stamp)
                        captured.callback(observer.close)
                        observers.append(observer)
                    self.assertLess(time.monotonic(), deadlines[0], "fixture capture exceeded original Git deadline")
                if leader_exits:
                    leader.sendall(b"x")
                capture_ack.set()
                worker.join(15)
                self.assertFalse(worker.is_alive(), "driver cleanup did not finish")
                self.assertEqual(errors, [])
                self.assertEqual(len(box), 1)
                expected_kind = "ok" if leader_exits else "timeout"
                self.assertEqual(box[0].kind, expected_kind, box[0].diagnostic)
                process_exit_observed = False
                if os.name == "nt":
                    self.assertEqual(len(observers), 2)
                    process_exit_observed = all(observer.wait_for_exit() for observer in observers)
                for connection in (peer, leader):
                    self.assert_owned_terminal_socket(
                        connection, box[0], expected_kind, cleanup_complete=True,
                        identity_ready=True, process_exit_observed=process_exit_observed,
                    )
        finally:
            # Never strand the test-only wait gate, including setup failures.
            capture_ack.set()
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
            worker.join(15)

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
        missing = ROOT / "does-not-exist"
        self.assertFalse(missing.exists())
        result = self.driver._run([str(missing)], timeout=5, env=os.environ.copy())
        self.assertEqual(result.kind, "spawn", result.diagnostic)
        self.assertEqual(result.returncode, None)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")
        self.assertEqual(self.driver._EXIT_CODES[result.kind], 3)
        self.assertTrue(result.diagnostic)

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
            missing_target = caller / "unavailable-checkout.git"
            gitfile = caller / ".git"
            gitfile_bytes = ("gitdir: " + str(missing_target) + "\n").encode("utf-8")
            gitfile.write_bytes(gitfile_bytes)
            self.assertFalse(missing_target.exists())
            broken = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=caller,
                                    env=environment, capture_output=True, timeout=10)
            self.assertEqual(broken.returncode, 128, broken.stderr)
            # Git diagnostics vary by version and locale; fixture state does not.
            self.assertEqual(gitfile.read_bytes(), gitfile_bytes)
            self.assertFalse(missing_target.exists())
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
            self.assertEqual(gitfile.read_bytes(), gitfile_bytes)
            self.assertFalse(missing_target.exists())
        self.assertEqual(environment, original_environment)
        self.assertEqual(pathlib.Path.cwd(), original_cwd)

    def test_unavailable_caller_diagnostics_do_not_bypass_actual_driver_assertions(self):
        actual_run = subprocess.run
        diagnostics = (
            ("legacy", b"fatal: Not a git repository (or any parent directory): .git\n"),
            ("path", b"fatal: not a git repository: /fixture/unavailable-checkout.git\n"),
            ("null", b"fatal: not a git repository: (null)\n"),
            ("localized", b"fatal: depot Git introuvable\n"),
        )
        for family, diagnostic in diagnostics:
            with self.subTest(diagnostic=family):
                probes = []
                workers = []

                def run_with_probe_diagnostic(arguments, **kwargs):
                    result = actual_run(arguments, **kwargs)
                    if arguments == ["git", "rev-parse", "--show-toplevel"]:
                        # Keep the real Git precondition and its actual status;
                        # vary only diagnostic text after the native probe.
                        self.assertEqual(result.returncode, 128, result.stderr)
                        result.stderr = diagnostic
                        probes.append(result)
                    elif arguments[:4] == [sys.executable, "-I", "-S", "-c"]:
                        workers.append(result)
                    return result

                with mock.patch.object(subprocess, "run", side_effect=run_with_probe_diagnostic):
                    self.test_owned_git_alias_context_does_not_move_worker_from_unavailable_caller_repo()
                self.assertEqual(len(probes), 1)
                self.assertEqual(len(workers), 1, "actual driver assertions were bypassed")
                self.assertEqual(workers[0].returncode, 0, workers[0].stderr)

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
