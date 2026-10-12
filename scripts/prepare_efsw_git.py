"""Own the processes used only for efsw Git preparation (Python 3.8+).

POSIX contains ordinary descendants in one private session/group. It does not
adopt grandchildren or contain descendants that deliberately call setsid().
Windows contains the command in a private kill-on-close job before resuming it.
The operation deadline includes command completion and output drain; cleanup
has its own bounded safety interval, never an unbounded Popen.wait().
"""
import sys

if __name__ == "__main__":
    if sys.version_info < (3, 8):
        sys.stderr.write("efsw Git preparation requires Python 3.8 or newer\n")
        sys.exit(3)
    if sys.argv[1:] == ["--check-interpreter"]:
        sys.exit(0)

import argparse
import json
import math
import os
import selectors
import shutil
import signal
import subprocess
import threading
import time
from dataclasses import dataclass

_CLEANUP_SECONDS = 5.0
_EXIT_CODES = {"ok": 0, "git": 1, "timeout": 2, "spawn": 3, "containment": 4, "cleanup": 5}


@dataclass
class _Outcome:
    kind: str
    returncode: object = None
    stdout: bytes = b""
    stderr: bytes = b""
    diagnostic: str = ""


class _Failure(Exception):
    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind


# Trusted private anchor code, not a public command/service. The anchor stays
# live until group termination, even after it has reaped Git. Never poll/reap
# the anchor before killpg: its reserved PID prevents reuse of our group ID.
_POSIX_ANCHOR = r'''
import json, os, signal, subprocess, sys
signal.signal(signal.SIGCHLD, signal.SIG_DFL)
status = int(sys.argv[1])
environment = json.loads(sys.stdin.buffer.readline())
def report(value):
    os.write(status, (json.dumps(value) + '\n').encode('utf-8'))
try:
    command = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL, env=environment)
except OSError as error:
    report({'error': str(error)})
else:
    report({'returncode': command.wait()})
sys.stdin.buffer.read()
'''


class _PosixOwner:
    def __init__(self):
        self.process = None
        self.status = None
        self.environment_message = b""
        self.stdout = self.stderr = None

    def start(self, argv, env, cwd):
        # An inherited ignored SIGCHLD can auto-reap the anchor on POSIX,
        # invalidating both original exit status and the reserved-PGID proof.
        if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
            raise _Failure("containment", "Git group ownership requires default SIGCHLD reaping disposition")
        read_fd, write_fd = os.pipe()
        try:
            self.process = subprocess.Popen(
                [sys.executable, "-I", "-S", "-c", _POSIX_ANCHOR, str(write_fd)] + list(argv),
                cwd=cwd, env=env, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False,
                start_new_session=True, pass_fds=(write_fd,),
            )
            self.stdout, self.stderr = self.process.stdout, self.process.stderr
            self.status = os.fdopen(read_fd, "rb", buffering=0)
            read_fd = None
            # Python startup may coerce LC_CTYPE. Deliver the caller's exact
            # copied environment over private IPC, never as command-line data.
            self.environment_message = (json.dumps(env) + "\n").encode("utf-8")
            os.set_blocking(self.process.stdin.fileno(), False)
        finally:
            os.close(write_fd)
            if read_fd is not None:
                os.close(read_fd)

    def wait(self, deadline):
        data = bytearray()
        sent = 0
        with selectors.DefaultSelector() as selector:
            selector.register(self.status, selectors.EVENT_READ)
            selector.register(self.process.stdin, selectors.EVENT_WRITE)
            while b"\n" not in data:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise _Failure("timeout", "Git operation deadline expired")
                events = selector.select(remaining)
                if not events:
                    raise _Failure("timeout", "Git operation deadline expired")
                for key, _ in events:
                    if key.fileobj is self.process.stdin:
                        try:
                            sent += os.write(key.fd, self.environment_message[sent:sent + 65536])
                        except BlockingIOError:
                            continue
                        if sent == len(self.environment_message):
                            selector.unregister(self.process.stdin)
                            self.environment_message = b""
                    else:
                        chunk = os.read(key.fd, 4096)
                        if not chunk:
                            raise _Failure("containment", "Git group owner exited without a command result")
                        data.extend(chunk)
        try:
            value = json.loads(data.split(b"\n", 1)[0])
            if "error" in value:
                raise _Failure("spawn", "Could not launch Git: " + value["error"])
            return int(value["returncode"])
        except (ValueError, KeyError, TypeError) as error:
            raise _Failure("containment", "Invalid Git group owner result: " + str(error))

    def cleanup(self, deadline):
        errors = []
        if self.process is not None:
            # One owned-group operation, while the anchor is still unreaped.
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except OSError as error:
                errors.append(str(error))
            try:
                self.process.wait(timeout=max(0.0, deadline - time.monotonic()))
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(str(error))
            self.process.stdin.close()
        if self.status is not None:
            self.status.close()
        if errors:
            raise OSError("; ".join(errors))


class _WindowsAPI:
    """Lazy native definitions: importing this module on POSIX needs no WinAPI."""
    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.c, self.w = ctypes, wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        pointer = ctypes.c_void_p
        size = ctypes.c_size_t
        handle = wintypes.HANDLE

        class STARTUPINFO(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                        ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
                        ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                        ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
                        ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
                        ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                        ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                        ("lpReserved2", pointer), ("hStdInput", handle),
                        ("hStdOutput", handle), ("hStdError", handle)]

        class STARTUPINFOEX(ctypes.Structure):
            _fields_ = [("startup", STARTUPINFO), ("attributes", pointer)]

        class PROCESS_INFORMATION(ctypes.Structure):
            _fields_ = [("process", handle), ("thread", handle),
                        ("process_id", wintypes.DWORD), ("thread_id", wintypes.DWORD)]

        class BASIC_LIMIT(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                        ("flags", wintypes.DWORD), ("min_working_set", size),
                        ("max_working_set", size), ("active_limit", wintypes.DWORD),
                        ("affinity", size), ("priority", wintypes.DWORD),
                        ("scheduling", wintypes.DWORD)]

        class IO_COUNTERS(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

        class EXTENDED_LIMIT(ctypes.Structure):
            _fields_ = [("basic", BASIC_LIMIT), ("io", IO_COUNTERS),
                        ("process_memory", size), ("job_memory", size),
                        ("peak_process_memory", size), ("peak_job_memory", size)]

        class COMPLETION_PORT(ctypes.Structure):
            _fields_ = [("key", pointer), ("port", handle)]

        self.STARTUPINFOEX = STARTUPINFOEX
        self.PROCESS_INFORMATION = PROCESS_INFORMATION
        self.EXTENDED_LIMIT = EXTENDED_LIMIT
        self.COMPLETION_PORT = COMPLETION_PORT
        signatures = {
            "CreateJobObjectW": ([pointer, wintypes.LPCWSTR], handle),
            "SetInformationJobObject": ([handle, ctypes.c_int, pointer, wintypes.DWORD], wintypes.BOOL),
            "CreateIoCompletionPort": ([handle, handle, size, wintypes.DWORD], handle),
            "GetQueuedCompletionStatus": ([handle, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(size), ctypes.POINTER(pointer), wintypes.DWORD], wintypes.BOOL),
            "InitializeProcThreadAttributeList": ([pointer, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(size)], wintypes.BOOL),
            "UpdateProcThreadAttribute": ([pointer, wintypes.DWORD, size, pointer, size, pointer, pointer], wintypes.BOOL),
            "DeleteProcThreadAttributeList": ([pointer], None),
            "GetCurrentProcess": ([], handle),
            "DuplicateHandle": ([handle, handle, handle, ctypes.POINTER(handle), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.BOOL),
            "CreateProcessW": ([wintypes.LPCWSTR, wintypes.LPWSTR, pointer, pointer, wintypes.BOOL, wintypes.DWORD, pointer, wintypes.LPCWSTR, pointer, pointer], wintypes.BOOL),
            "IsProcessInJob": ([handle, handle, ctypes.POINTER(wintypes.BOOL)], wintypes.BOOL),
            "ResumeThread": ([handle], wintypes.DWORD),
            "WaitForSingleObject": ([handle, wintypes.DWORD], wintypes.DWORD),
            "GetExitCodeProcess": ([handle, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            "TerminateJobObject": ([handle, wintypes.UINT], wintypes.BOOL),
            "CloseHandle": ([handle], wintypes.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result

    def error(self, operation):
        code = self.c.get_last_error()
        return _Failure("containment", "{} failed (Windows error {}: {}). "
                        "This build host must support atomic job-list process containment; no fallback is permitted.".format(
                            operation, code, self.c.FormatError(code).strip()))

    def close(self, handle):
        if handle and not self.kernel.CloseHandle(handle):
            raise self.error("CloseHandle")

    def create_job(self):
        c, w, k = self.c, self.w, self.kernel
        job = k.CreateJobObjectW(None, None)
        if not job:
            raise self.error("CreateJobObjectW")
        port = None
        try:
            limits = self.EXTENDED_LIMIT()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not k.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)):
                raise self.error("SetInformationJobObject(KILL_ON_JOB_CLOSE)")
            port = k.CreateIoCompletionPort(w.HANDLE(-1), None, 0, 1)
            if not port:
                raise self.error("CreateIoCompletionPort")
            association = self.COMPLETION_PORT(c.c_void_p(1), port)
            if not k.SetInformationJobObject(job, 7, c.byref(association), c.sizeof(association)):
                raise self.error("SetInformationJobObject(completion port)")
            return job, port
        except Exception as original:
            cleanup_errors = []
            for owned in (job, port):
                if owned:
                    try:
                        self.close(owned)
                    except Exception as error:
                        cleanup_errors.append(str(error))
            if cleanup_errors:
                raise _Failure("cleanup", str(original) + "; native job cleanup failed: " + "; ".join(cleanup_errors))
            raise

    def create_suspended(self, owner, argv, env, cwd, job):
        import msvcrt
        c, w, k = self.c, self.w, self.kernel
        pipes = []
        inherited = []
        attributes = None
        initialized = False
        process = self.PROCESS_INFORMATION()
        original = None
        try:
            null_fd = os.open(os.devnull, os.O_RDONLY)
            pipes.append(null_fd)
            out_read, out_write = os.pipe()
            pipes.extend((out_read, out_write))
            err_read, err_write = os.pipe()
            pipes.extend((err_read, err_write))
            current = k.GetCurrentProcess()
            for fd in (null_fd, out_write, err_write):
                duplicate = w.HANDLE()
                if not k.DuplicateHandle(current, msvcrt.get_osfhandle(fd), current,
                                         c.byref(duplicate), 0, True, 2):
                    raise self.error("DuplicateHandle(stdio)")
                inherited.append(duplicate.value)
            size = c.c_size_t()
            k.InitializeProcThreadAttributeList(None, 2, 0, c.byref(size))
            if not size.value:
                raise self.error("InitializeProcThreadAttributeList(size)")
            attributes = c.create_string_buffer(size.value)
            if not k.InitializeProcThreadAttributeList(attributes, 2, 0, c.byref(size)):
                raise self.error("InitializeProcThreadAttributeList")
            initialized = True
            handles = (w.HANDLE * 3)(*inherited)
            jobs = (w.HANDLE * 1)(job)
            # HANDLE_LIST restricts inheritance. JOB_LIST assigns atomically:
            # failed creation never exposes an unassigned suspended child that
            # would require a forbidden direct-process termination fallback.
            for attribute, value in ((0x20002, handles), (0x2000D, jobs)):
                if not k.UpdateProcThreadAttribute(attributes, 0, attribute, c.byref(value), c.sizeof(value), None, None):
                    raise self.error("UpdateProcThreadAttribute({})".format(
                        "HANDLE_LIST" if attribute == 0x20002 else "JOB_LIST"))
            startup = self.STARTUPINFOEX()
            startup.startup.cb = c.sizeof(startup)
            startup.startup.dwFlags = 0x100  # STARTF_USESTDHANDLES
            startup.startup.hStdInput, startup.startup.hStdOutput, startup.startup.hStdError = inherited
            startup.attributes = c.cast(attributes, c.c_void_p)
            command = c.create_unicode_buffer(subprocess.list2cmdline(argv))
            environment = c.create_unicode_buffer("\0".join(
                "{}={}".format(key, value) for key, value in sorted(env.items(), key=lambda item: item[0].upper())
            ) + "\0\0")
            flags = 0x00000004 | 0x00000400 | 0x00080000  # suspended, Unicode env, extended startup
            # Publish all resources into the owner before any post-create
            # operation can fail, including inherited-handle cleanup.
            owner.stdout = os.fdopen(out_read, "rb", buffering=0)
            pipes.remove(out_read)
            owner.stderr = os.fdopen(err_read, "rb", buffering=0)
            pipes.remove(err_read)
            if not k.CreateProcessW(argv[0], command, None, None, True, flags,
                                    environment, cwd, c.byref(startup), c.byref(process)):
                code = c.get_last_error()
                error = self.error("CreateProcessW(atomic JOB_LIST)")
                if code in (2, 3, 193, 216):  # missing path or invalid executable
                    error.kind = "spawn"
                raise error
            owner.process, owner.thread = process.process, process.thread
        except Exception as error:
            original = error
            raise
        finally:
            cleanup_errors = []
            if initialized:
                k.DeleteProcThreadAttributeList(attributes)
            for handle in inherited:
                try:
                    self.close(handle)
                except Exception as error:
                    cleanup_errors.append(str(error))
            for fd in pipes:
                try:
                    os.close(fd)
                except OSError as error:
                    cleanup_errors.append(str(error))
            if cleanup_errors:
                primary = (str(original) + "; ") if original is not None else ""
                raise _Failure("cleanup", primary + "native stdio cleanup failed: " + "; ".join(cleanup_errors))

    def verify_job(self, process, job):
        member = self.w.BOOL()
        if not self.kernel.IsProcessInJob(process, job, self.c.byref(member)):
            raise self.error("IsProcessInJob")
        if not member.value:
            raise _Failure("containment", "Suspended Git process is not in the owned job; refusing resume")

    def resume(self, thread):
        previous = self.kernel.ResumeThread(thread)
        if previous != 1:
            if previous == 0xFFFFFFFF:
                raise self.error("ResumeThread")
            raise _Failure("containment", "Unexpected suspended Git thread state: {}".format(previous))

    def wait(self, process, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise _Failure("timeout", "Git operation deadline expired")
        milliseconds = min(0xFFFFFFFE, max(1, math.ceil(remaining * 1000)))
        status = self.kernel.WaitForSingleObject(process, milliseconds)
        if status == 0x102:
            raise _Failure("timeout", "Git operation deadline expired")
        if status != 0:
            raise self.error("WaitForSingleObject")
        code = self.w.DWORD()
        if not self.kernel.GetExitCodeProcess(process, self.c.byref(code)):
            raise self.error("GetExitCodeProcess")
        return code.value

    def terminate(self, job):
        if not self.kernel.TerminateJobObject(job, 1):
            raise self.error("TerminateJobObject")

    def quiescence(self, port, deadline):
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise _Failure("cleanup", "Owned Git job did not report ACTIVE_PROCESS_ZERO")
            message = self.w.DWORD()
            key = self.c.c_size_t()
            overlapped = self.c.c_void_p()
            milliseconds = min(0xFFFFFFFE, max(1, math.ceil(remaining * 1000)))
            if not self.kernel.GetQueuedCompletionStatus(port, self.c.byref(message), self.c.byref(key),
                                                         self.c.byref(overlapped), milliseconds):
                raise self.error("GetQueuedCompletionStatus(ACTIVE_PROCESS_ZERO)")
            if key.value == 1 and message.value == 4:  # JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO
                return


class _WindowsOwner:
    def __init__(self, _api_factory=None):
        self._api_factory = _api_factory or _WindowsAPI
        self.api = None
        self.job = self.port = self.process = self.thread = None
        self.stdout = self.stderr = None

    def start(self, argv, env, cwd):
        try:
            self.api = self._api_factory()
        except Exception as error:
            raise _Failure("containment", "Windows job containment APIs are unavailable: " + str(error))
        self.job, self.port = self.api.create_job()
        self.api.create_suspended(self, argv, env, cwd, self.job)
        self.api.verify_job(self.process, self.job)
        self.api.resume(self.thread)

    def wait(self, deadline):
        return self.api.wait(self.process, deadline)

    def cleanup(self, deadline):
        errors = []
        if self.api is None:
            return
        try:
            if self.job is not None:
                try:
                    self.api.terminate(self.job)
                except Exception as error:
                    errors.append(str(error))
                if self.process is not None:
                    try:
                        self.api.quiescence(self.port, deadline)
                        self.api.wait(self.process, deadline)
                    except Exception as error:
                        errors.append(str(error))
        finally:
            # Close the job even on errors: KILL_ON_JOB_CLOSE is the backstop,
            # not a substitute for observed semantic quiescence on success.
            for handle in (self.thread, self.process, self.job, self.port):
                if handle is not None:
                    try:
                        self.api.close(handle)
                    except Exception as error:
                        errors.append(str(error))
        if errors:
            raise _Failure("cleanup", "; ".join(errors))


def _environment(operation, global_config, ceiling, original=None):
    environment = dict(os.environ if original is None else original)
    if operation == "config":
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
            environment.pop(name, None)
        environment["GIT_CEILING_DIRECTORIES"] = ceiling
    elif operation == "clone":
        for name in ("GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT"):
            environment.pop(name, None)
        environment["GIT_CONFIG_GLOBAL"] = global_config
    return environment


def _run(argv, *, timeout, env, cwd=None, _owner_factory=None):
    """Private fixture seam; the CLI always resolves and invokes only Git."""
    deadline = time.monotonic() + timeout
    owner = (_owner_factory or (_WindowsOwner if os.name == "nt" else _PosixOwner))()
    result = _Outcome("ok")
    buffers = [bytearray(), bytearray()]
    readers = []
    read_errors = []
    drained_at = [None, None]
    command_finished_at = None
    started = False

    def drain(pipe, buffer, index):
        try:
            while True:
                chunk = os.read(pipe.fileno(), 65536)
                if not chunk:
                    drained_at[index] = time.monotonic()
                    break
                buffer.extend(chunk)
        except OSError as error:
            read_errors.append(str(error))
        finally:
            # Only the now-unblocked reader closes its FD. On Windows a close
            # from another thread can wait indefinitely for the CRT read lock.
            try:
                pipe.close()
            except OSError as error:
                read_errors.append(str(error))

    try:
        try:
            owner.start(list(argv), dict(env), cwd)
            started = True
        finally:
            # Partially started owners still own pipes and must drain them.
            for index, (pipe, buffer) in enumerate(zip((owner.stdout, owner.stderr), buffers)):
                if pipe is not None:
                    reader = threading.Thread(target=drain, args=(pipe, buffer, index), daemon=True)
                    reader.start()
                    readers.append((reader, pipe))
                else:
                    drained_at[index] = time.monotonic()
        result.returncode = owner.wait(deadline)
        command_finished_at = time.monotonic()
        if result.returncode != 0:
            result.kind = "git"
            result.diagnostic = "Git exited with status {}".format(result.returncode)
    except _Failure as error:
        result.kind, result.diagnostic = error.kind, str(error)
    except OSError as error:
        result.kind = "containment" if started else "spawn"
        result.diagnostic = ("Git owner wait failed: " if started else "Could not launch Git owner: ") + str(error)
    except Exception as error:
        result.kind, result.diagnostic = "containment", "Git owner failed: " + str(error)
    finally:
        cleanup_deadline = time.monotonic() + _CLEANUP_SECONDS
        cleanup_errors = []
        try:
            owner.cleanup(cleanup_deadline)
        except Exception as error:
            cleanup_errors.append(str(error))
        # EOF timestamps, not cleanup's end time, define operation completion.
        # A slow but valid safety cleanup must not retroactively time out Git.
        for reader, _ in readers:
            reader.join(max(0.0, cleanup_deadline - time.monotonic()))
        if any(reader.is_alive() for reader, _ in readers):
            cleanup_errors.append("Git output did not reach EOF during bounded cleanup")
        if result.kind in ("ok", "git") and (
            command_finished_at is None or command_finished_at > deadline or
            any(finished is None or finished > deadline for finished in drained_at)
        ):
            result.kind = "timeout"
            result.diagnostic += "\nGit operation deadline expired during output drain"
        cleanup_errors.extend(read_errors)
        for pipe in (owner.stdout, owner.stderr):
            # A blocked daemon reader retains ownership until it can close the
            # FD itself. Report cleanup failure; never turn it into a hang.
            blocked = any(reader.is_alive() and owned_pipe is pipe for reader, owned_pipe in readers)
            if pipe is not None and not blocked:
                try:
                    pipe.close()
                except OSError as error:
                    cleanup_errors.append(str(error))
        if cleanup_errors:
            result.kind = "cleanup"
            result.diagnostic += "\nGit cleanup failed: " + "; ".join(cleanup_errors)
    result.stdout, result.stderr = bytes(buffers[0]), bytes(buffers[1])
    return result


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise _Failure("spawn", "Invalid Git driver usage: " + message)


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    if sys.version_info < (3, 8):
        sys.stderr.write("efsw Git preparation requires Python 3.8 or newer\n")
        return 3
    if arguments == ["--check-interpreter"]:
        return 0
    parser = _Parser(description=__doc__)
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("--operation", choices=("inherit", "config", "clone"), required=True)
    parser.add_argument("--global-config")
    parser.add_argument("--ceiling")
    parser.add_argument("--startup-lc-ctype-state", choices=("absent", "present"), required=True)
    parser.add_argument("--startup-lc-ctype")
    try:
        if "--" not in arguments:
            parser.error("expected -- followed by Git arguments")
        separator = arguments.index("--")
        options = parser.parse_args(arguments[:separator])
        git_arguments = arguments[separator + 1:]
        if not git_arguments:
            parser.error("Git arguments must not be empty")
        if not math.isfinite(options.timeout) or options.timeout <= 0:
            parser.error("--timeout must be finite and positive")
        if options.operation == "config" and options.ceiling is None:
            parser.error("config requires --ceiling")
        if options.operation == "clone" and options.global_config is None:
            parser.error("clone requires --global-config")
        if (options.startup_lc_ctype_state == "present") != (options.startup_lc_ctype is not None):
            parser.error("--startup-lc-ctype is required only for the present startup locale state")
        if os.name == "posix":
            try:
                # This changes only our owned CLI interpreter, never the
                # launching CMake process or a library caller's signal state.
                signal.signal(signal.SIGCHLD, signal.SIG_DFL)
            except (OSError, ValueError) as error:
                raise _Failure("containment", "Could not establish Git child reaping: " + str(error))
        git = shutil.which("git")
        if git is None:
            raise _Failure("spawn", "Could not find Git on PATH")
        environment = _environment(options.operation, options.global_config, options.ceiling)
        # The caller captures this nonsecret selector before Python startup.
        # PEP 538 can add/change LC_CTYPE before our first os.environ snapshot.
        if options.startup_lc_ctype_state == "absent":
            environment.pop("LC_CTYPE", None)
        else:
            environment["LC_CTYPE"] = options.startup_lc_ctype
        result = _run([git] + git_arguments, timeout=options.timeout, env=environment)
    except _Failure as error:
        sys.stderr.write(str(error) + "\n")
        return _EXIT_CODES[error.kind]
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    if result.diagnostic:
        sys.stderr.buffer.write(("\nefsw Git driver: " + result.diagnostic + "\n").encode("utf-8"))
    return _EXIT_CODES[result.kind]


if __name__ == "__main__":
    sys.exit(main())
