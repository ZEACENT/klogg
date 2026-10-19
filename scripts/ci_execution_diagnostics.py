#!/usr/bin/env python3
"""Opt-in macOS TSan snapshots, independent of the authoritative build runner.

All records are sampled/partial, including the stop record. Short-lived processes
can disappear between samples; ps CPU percentages are not interval accounting.
The parent owns a control pipe, so cancellation/parent death also stops sampling.
Only this diagnostic process group can be terminated by its lifecycle manager.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time

INTERVAL_SECONDS = 15
PROBE_TIMEOUT_SECONDS = 1
PROBE_BYTES = 256 * 1024
JOURNAL_BYTES = 4 * 1024 * 1024  # One current file and one rotated predecessor.
CHECKPOINT_BYTES = 2 * 1024 * 1024
PREFIX = "KLOGG_BUILD_DIAGNOSTIC_JSON="
PS_COMMAND = ["/bin/ps", "-axo", "pid=,ppid=,%cpu=,rss=,state=,time=,comm="]
PROCESS_ROLES = {**dict.fromkeys(("clang", "clang++", "cc", "c++", "gcc", "g++", "cc1", "cc1plus"), "compiler"),
                 **dict.fromkeys(("ccache", "sccache"), "cache"),
                 **dict.fromkeys(("cmake", "ninja", "make", "gmake"), "build-tool")}
COUNTERS = frozenset(("direct_cache_hit", "preprocessed_cache_hit", "cache_miss",
                      "cache_size_kib", "files_in_cache", "stats_zeroed_timestamp",
                      "cleanups_performed", "called_for_link"))
VM_FIELDS = {"Pages free": "pages_free", "Pages active": "pages_active",
             "Pages inactive": "pages_inactive", "Pages wired down": "pages_wired",
             "Pages occupied by compressor": "pages_compressor", "Pageins": "pageins",
             "Pageouts": "pageouts", "Swapins": "swapins", "Swapouts": "swapouts",
             "Compressions": "compressions", "Decompressions": "decompressions"}


def positive_integer(value):
    return int(value) if isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{0,8}", value) else None


def tool_version(tool, text):
    prefixes = {"cmake": r"cmake version ", "ninja": "", "ccache": r"ccache version "}
    match = re.match(prefixes[tool] + r"([0-9]{1,8}(?:\.[0-9]{1,8}){0,3})(?:\s|$)", text or "")
    return [int(value) for value in match[1].split(".")] if match else None


def cache_counters(text):
    values = {}
    for line in (text or "").splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] in COUNTERS and re.fullmatch(r"[0-9]{1,19}", fields[1]):
            values[fields[0]] = int(fields[1])
    return values or None


def cache_configuration(text):
    """Never retain config origins, remote URLs, or custom compiler-check commands."""
    values = {}
    for line in (text or "").splitlines():
        match = re.fullmatch(r"(?:\([^)]*\)\s+)?([a-z_]+) = (.*)", line)
        if not match:
            continue
        name, value = match.groups()
        if name == "compiler_check":
            values[name] = value if value in ("content", "mtime", "none") else "custom"
        elif name == "compression" and value in ("true", "false"):
            values[name] = value == "true"
        elif name == "compression_level" and re.fullmatch(r"-?[0-9]{1,3}", value):
            values[name] = int(value)
        elif name == "remote_storage":
            values["remote_storage_enabled"] = bool(value.strip())
        elif name == "max_size":
            size = re.fullmatch(r"([0-9]{1,12}(?:\.[0-9]{1,6})?)\s*([kKMGT]?)(i?)(?:B)?", value)
            if size:
                amount, unit, binary = size.groups()
                power = " KMGT".index(unit.upper() or " ")
                values["max_size_bytes"] = round(float(amount) * (1024 if binary else 1000) ** power)
    return values or None


def vm_facts(text):
    values = {}
    match = re.search(r"page size of ([0-9]{1,9}) bytes", text or "")
    if match:
        values["page_size_bytes"] = int(match[1])
    for line in (text or "").splitlines():
        match = re.fullmatch(r"([^:]+):\s*([0-9]{1,19})\.?", line)
        if match and match[1] in VM_FIELDS:
            values[VM_FIELDS[match[1]]] = int(match[2])
    return values or None


def swap_facts(text):
    values = {}
    for name, value, unit in re.findall(r"\b(total|used|free)\s*=\s*([0-9]{1,12}(?:\.[0-9]{1,6})?)([KMGT])", text or ""):
        values[name + "_bytes"] = round(float(value) * 1024 ** ("KMGT".index(unit) + 1))
    return values or None


def descendants(text, parent, sampler, max_rows=32):
    processes, children = {}, {}
    for line in text.splitlines():
        fields = line.split(maxsplit=6)
        if len(fields) != 7:
            continue
        pid, ppid, cpu, rss, state, elapsed, executable = fields
        if (not re.fullmatch(r"[0-9]{1,9}", pid) or not re.fullmatch(r"[0-9]{1,9}", ppid)
                or not re.fullmatch(r"[0-9]{1,16}", rss)
                or not re.fullmatch(r"[0-9]{1,6}(?:\.[0-9]{1,6})?", cpu)
                or state[:1] not in "RSDIUTZX?" or not state):
            continue
        match = re.fullmatch(r"(?:(\d{1,6})-)?(?:(\d{1,6}):)?(\d{1,6}):(\d{1,6}(?:\.\d{1,6})?)", elapsed)
        if not match:
            continue
        days, hours, minutes, seconds = (float(value or 0) for value in match.groups())
        pid, ppid = int(pid), int(ppid)
        processes[pid] = {"pid": pid, "ppid": ppid, "ps_cpu_percent": float(cpu),
                          "rss_kib": int(rss), "state": state[0],
                          "role": PROCESS_ROLES.get(executable.rsplit("/", 1)[-1], "other"),
                          "cpu_seconds": ((days * 24 + hours) * 60 + minutes) * 60 + seconds}
        children.setdefault(ppid, []).append(pid)
    selected, pending, visited = [], list(children.get(parent, [])), {parent, sampler}
    while pending:
        pid = pending.pop()
        if pid in visited:
            continue
        visited.add(pid)
        selected.append(processes[pid])
        pending.extend(children.get(pid, []))
    selected.sort(key=lambda row: (-row["rss_kib"], row["pid"]))
    return {"observed_count": len(selected), "rows_truncated": len(selected) > max_rows,
            "rss_kib_sum": sum(row["rss_kib"] for row in selected),
            "ps_cpu_percent_sum": round(sum(row["ps_cpu_percent"] for row in selected), 3),
            "processes": selected[:max_rows]}


class Probe:
    """Fixed read-only commands; bound output, execution, and cleanup separately."""
    def __init__(self, stopped, timeout=PROBE_TIMEOUT_SECONDS):
        self.stopped = stopped
        self.timeout = timeout
        self.failures = 0

    def run(self, command):
        if self.stopped():
            return None
        process = None
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + self.timeout
            data = bytearray()
            while not self.stopped():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                readable, _, _ = select.select([process.stdout], [], [], min(remaining, 0.1))
                if not readable:
                    continue
                chunk = os.read(process.stdout.fileno(), min(8192, PROBE_BYTES + 1 - len(data)))
                if not chunk:
                    # EOF can precede process exit. Classify one observed status,
                    # never re-poll a changing value inside the same decision.
                    status = process.poll()
                    if status == 0:
                        return data.decode("utf-8", errors="replace")
                    if status is not None:
                        break
                    continue
                data.extend(chunk)
                if len(data) > PROBE_BYTES:
                    break
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        finally:
            if process is not None:
                try:
                    if process.poll() is None:
                        process.kill()  # Only the diagnostic command, never a build PID.
                    process.wait(timeout=0.2)
                except (OSError, subprocess.SubprocessError):
                    pass
                if process.stdout is not None:
                    process.stdout.close()
        self.failures += 1
        return None


class Journal:
    def __init__(self, root, max_bytes=JOURNAL_BYTES):
        self.path = Path(root) / "ci-build-diagnostics.jsonl"
        self.maximum = max_bytes
        self.stream = self.path.open("x", encoding="utf-8")
        self.size = 0
        self.sequence = 0
        self.started = time.monotonic()

    def emit(self, fields):
        record = {"schema_version": 1, "scope": "sampled-partial", "sequence": self.sequence,
                  "elapsed_ms": round((time.monotonic() - self.started) * 1000), **fields}
        line = json.dumps(record, sort_keys=True, allow_nan=False, ensure_ascii=True) + "\n"
        if len(line.encode("utf-8")) > min(self.maximum, 16384):
            raise ValueError("diagnostic record exceeds bound")
        if self.size + len(line) > self.maximum:
            self.stream.close()
            os.replace(self.path, self.path.with_suffix(".jsonl.previous"))
            self.stream = self.path.open("x", encoding="utf-8")
            self.size = 0
        self.stream.write(line)
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.size += len(line)
        self.sequence += 1
        print(PREFIX + line.rstrip("\n"), flush=True)

    def close(self):
        self.stream.close()


def checkpoint(root, max_bytes=CHECKPOINT_BYTES):
    root = Path(root)
    temporary = None
    try:
        fd = os.open(root / ".ninja_log", os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode):
                return {"status": "unavailable"}
            offset = max(0, info.st_size - max_bytes)
            source.seek(offset)
            data = source.read(max_bytes)
        if offset:
            boundary = data.find(b"\n")
            if boundary < 0:
                return {"status": "unavailable"}
            offset += boundary + 1
            data = data[boundary + 1:]
        with tempfile.NamedTemporaryFile(dir=root, prefix=".ci-diagnostics-", delete=False) as target:
            temporary = Path(target.name)
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, root / "ci-build-diagnostics.ninja-log.txt")
        completed = [int(fields[1]) for line in data.splitlines() if len(fields := line.split(b"\t")) == 5
                     and fields[0].isdigit() and fields[1].isdigit()]
        return {"status": "available", "source_size_bytes": info.st_size, "copied_bytes": len(data),
                "source_offset_bytes": offset, "truncated": offset != 0,
                "trailing_partial_line": bool(data and not data.endswith(b"\n")),
                "observed_records": len(completed), "latest_completed_ms": max(completed, default=None)}
    except OSError:
        return {"status": "unavailable"}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def initial_facts(root, probe):
    text = probe.run(["/usr/sbin/sysctl", "-n", "hw.logicalcpu", "hw.physicalcpu", "hw.memsize"])
    values = (text or "").splitlines()
    hardware = dict(zip(("logical_cpus", "physical_cpus", "ram_bytes"),
                        (positive_integer(value) for value in values))) if len(values) == 3 else None
    # RAM commonly exceeds the bounded parallelism parser's nine-digit limit.
    if hardware is not None:
        hardware["ram_bytes"] = int(values[2]) if re.fullmatch(r"[0-9]{1,19}", values[2]) else None
    versions = {tool: tool_version(tool, probe.run([tool, "--version"])) for tool in ("cmake", "ninja", "ccache")}
    compilers = {}
    for language in ("C", "CXX"):
        try:
            paths = list(Path(root).glob(f"CMakeFiles/*/CMake{language}Compiler.cmake"))
            if len(paths) == 1 and paths[0].stat().st_size <= 65536:
                text = paths[0].read_text(encoding="utf-8")
                match = re.search(r'^set\(CMAKE_' + language + r'_COMPILER_VERSION "([0-9]{1,8}(?:\.[0-9]{1,8}){0,3})"\)$', text, re.M)
                if match:
                    compilers[language] = [int(value) for value in match[1].split(".")]
        except (OSError, UnicodeError):
            continue
    configuration = cache_configuration(probe.run(["ccache", "--show-config"]))
    return {"kind": "initial", "hardware": hardware, "tool_versions": versions,
            "compiler_versions": compilers, "effective_ccache_config": configuration,
            "probe_failures": probe.failures}


def sample(root, parent, probe):
    text = probe.run(PS_COMMAND)
    try:
        load = list(os.getloadavg())
        if not all(math.isfinite(value) for value in load):
            load = None
    except OSError:
        load = None
    vm = vm_facts(probe.run(["/usr/bin/vm_stat"]))
    swap = swap_facts(probe.run(["/usr/sbin/sysctl", "-n", "vm.swapusage"]))
    counters = cache_counters(probe.run(["ccache", "--print-stats"]))
    return {"kind": "sample", "processes": descendants(text, parent, os.getpid()) if text is not None else None,
            "load_1_5_15": load, "vm": vm, "swap": swap, "ccache_counters": counters,
            "probe_failures": probe.failures, "ninja": checkpoint(root)}


def watch(root, parent, control_fd=0):
    stopping = False
    def request_stop(_signum, _frame):
        nonlocal stopping
        stopping = True
    def stopped():
        nonlocal stopping
        if not stopping and select.select([control_fd], [], [], 0)[0]:
            os.read(control_fd, 1)  # A byte or EOF both mean the owner is gone/done.
            stopping = True
        return stopping
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    journal = None
    try:
        journal = Journal(root)
        journal.emit({"kind": "start", "parent_pid": parent, "sampler_pid": os.getpid(),
                      "sample_interval_seconds": INTERVAL_SECONDS,
                      "probe_timeout_seconds": PROBE_TIMEOUT_SECONDS,
                      "declared_parallelism": positive_integer(os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")),
                      "declared_parallelism_source": "CMAKE_BUILD_PARALLEL_LEVEL",
                      "short_lived_processes_may_be_missing": True})
        probe = Probe(stopped)
        if not stopped():
            journal.emit(initial_facts(root, probe))
        while not stopped():
            journal.emit(sample(root, parent, probe))
            next_sample = time.monotonic() + INTERVAL_SECONDS
            while not stopped():
                remaining = next_sample - time.monotonic()
                if remaining <= 0:
                    break
                # Python restarts select after handled signals. Keep signal-only
                # shutdown bounded even while the owner's pipe remains open.
                select.select([control_fd], [], [], min(remaining, 0.1))
        journal.emit({"kind": "stop", "ninja": checkpoint(root)})
    except Exception:
        # Never stringify probe exceptions: they may contain paths or credentials.
        print(PREFIX + '{"schema_version":1,"scope":"sampled-partial","kind":"unavailable"}', flush=True)
    finally:
        if journal is not None:
            journal.close()
    return 0


class Session:
    def __init__(self, process):
        self.process = process

    def stop(self):
        process = self.process
        if process.stdin is not None:
            process.stdin.close()
        try:
            process.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            # The sampler is a separate session containing diagnostic probes only.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=0.2)


def start(build_root, mode):
    if mode != "macos-tsan" or sys.platform != "darwin" or os.environ.get("KLOGG_SANITIZER") != "thread":
        return None
    process = subprocess.Popen([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                "--watch", str(Path(build_root).resolve()), str(os.getpid())],
                               stdin=subprocess.PIPE, start_new_session=True)
    return Session(process)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] != "--watch" or positive_integer(sys.argv[3]) is None:
        raise SystemExit(2)
    raise SystemExit(watch(Path(sys.argv[2]), int(sys.argv[3])))
