#!/usr/bin/env python3
"""Optional, hash-pinned Windows object cache with validated, namespace-bound saves."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK = ROOT / "ci/tools/sccache-windows.json"
PRELOAD = ROOT / "cmake/WindowsObjectCache.cmake"
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_EXE_BYTES = 128 * 1024 * 1024
NATIVE_WINDOWS = os.name == "nt"
STATE_NAME = "windows-object-cache-state.json"
REPORT_NAME = "windows-object-cache.json"


def digest(path):
    with pathlib.Path(path).open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def load_lock(path=LOCK):
    lock = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(lock, dict):
        raise ValueError("invalid Windows sccache lock")
    version = lock.get("version", "")
    stem = f"sccache-v{version}-x86_64-pc-windows-msvc"
    if (lock.get("schema_version") != 1 or not isinstance(version, str)
            or not re.fullmatch(r"\d+\.\d+\.\d+", version)
            or lock.get("archive_url") != f"https://github.com/mozilla/sccache/releases/download/v{version}/{stem}.zip"
            or lock.get("member") != f"{stem}/sccache.exe"
            or not isinstance(lock.get("archive_sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", lock["archive_sha256"])
            or not isinstance(lock.get("cache_budget_bytes"), dict)
            or any(type(size) is not int or size <= 0 for size in lock["cache_budget_bytes"].values())):
        raise ValueError("invalid Windows sccache lock")
    return lock


def install(lock, temporary_root):
    # Never execute from an archive directory, or cache an executable alongside
    # PR-restorable objects. Only the exact hash-verified member is materialized.
    with tempfile.TemporaryDirectory(prefix="sccache-download-", dir=temporary_root) as directory:
        archive = pathlib.Path(directory) / "archive.zip"
        request = urllib.request.Request(lock["archive_url"], headers={"User-Agent": "klogg-ci-cache/1"})
        with urllib.request.urlopen(request, timeout=30) as response, archive.open("wb") as target:
            total = 0
            while chunk := response.read(min(1024 * 1024, MAX_ARCHIVE_BYTES - total + 1)):
                total += len(chunk)
                if total > MAX_ARCHIVE_BYTES:
                    raise ValueError("sccache download exceeds byte limit")
                target.write(chunk)
        if digest(archive) != lock["archive_sha256"]:
            raise ValueError("sccache archive SHA-256 mismatch")
        with zipfile.ZipFile(archive) as source:
            selected = [entry for entry in source.infolist() if entry.filename == lock["member"]]
            if len(selected) != 1 or selected[0].file_size > MAX_EXE_BYTES:
                raise ValueError("invalid sccache executable member")
            data = source.read(selected[0])
            if not data.startswith(b"MZ"):
                raise ValueError("sccache executable is not a PE image")
        destination = pathlib.Path(tempfile.mkdtemp(prefix="sccache-tool-", dir=temporary_root))
        executable = destination / "sccache.exe"
        executable.write_bytes(data)
        return executable


def cache_identity(env, lock, compiler, preload=PRELOAD):
    # Windows os.environ is case-insensitive and iterates uppercase names. Once
    # copied to dict, mixed-case VS lookups stop working. Normalize lookup keys,
    # not values or serialized identity names: real toolchain changes must still
    # change the fingerprint, and spelling alone must not create another cache.
    windows_env = {}
    for key, value in env.items():
        name = key.upper()
        if name in windows_env and windows_env[name] != value:
            raise ValueError(f"ambiguous Windows environment variable: {name}")
        windows_env[name] = value
    required = ("KLOGG_CONFIG_ARCH", "KLOGG_QT_VERSION", "KLOGG_QT_ARCH", "KLOGG_LABEL",
                "VCToolsVersion", "WindowsSDKVersion", "VSCMD_ARG_TGT_ARCH")
    missing = [key for key in required if not windows_env.get(key.upper())]
    if missing:
        raise ValueError("missing Windows compiler/toolchain identity: " + ", ".join(missing))
    arch = windows_env["KLOGG_CONFIG_ARCH"]
    if arch not in {"x64", "x86"} or windows_env["VSCMD_ARG_TGT_ARCH"] != arch:
        raise ValueError("MSVC target architecture mismatch")
    if "-DCMAKE_BUILD_TYPE=RelWithDebInfo" not in windows_env.get("KLOGG_CMAKE_OPTS", "").split():
        raise ValueError("Windows object cache requires RelWithDebInfo")
    identity = {key: windows_env[key.upper()] for key in required}
    identity.update({"compiler_sha256": digest(compiler), "config": "RelWithDebInfo",
                     "cmake_options": windows_env["KLOGG_CMAKE_OPTS"],
                     "sanitizer": windows_env.get("KLOGG_SANITIZER", ""),
                     "sccache_sha256": lock["archive_sha256"], "preload_sha256": digest(preload)})
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:32]
    return f"windows-sccache-v1-{fingerprint}", identity


def cache_keys(base, event, ref, number, day):
    master = base + "-master-"
    if event == "pull_request":
        if not re.fullmatch(r"[1-9][0-9]*", str(number)):
            raise ValueError("invalid PR cache namespace")
        primary = base + f"-pr-{number}-"
        writable = True
    elif event in {"push", "workflow_dispatch"}:
        primary = master
        writable = event == "push" and ref == "refs/heads/master"
    else:
        raise ValueError("unsupported cache event")
    return {"primary-prefix": primary, "master-prefix": master,
            "primary-key": primary + day, "master-key": master + day,
            "save-key": primary + day if writable else ""}


def emit(path, values):
    if not path:
        return
    with pathlib.Path(path).open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            value = str(value)
            if "\n" in value or "\r" in value:
                raise ValueError("newline in Actions command file value")
            stream.write(f"{key}={value}\n")


def run(executable, *args, env):
    result = subprocess.run([str(executable), *args], env=env, text=True,
                            capture_output=True, timeout=90, check=False)
    if result.returncode:
        raise RuntimeError(f"sccache {args[0]} failed: {result.stderr.strip()} {result.stdout.strip()}")
    return result.stdout


def run_identity(env):
    return {key: env.get(key, "") for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_SHA")}


def prepare(workspace, env):
    state = {"enabled": False, "ready": False, "run": run_identity(env)}
    emit(env.get("GITHUB_ENV"), {"KLOGG_WINDOWS_OBJECT_CACHE": "disabled"})
    try:
        if not NATIVE_WINDOWS:
            raise ValueError("Windows object cache requires native Windows Python")
        lock = load_lock()
        compiler = shutil.which("cl.exe")
        if not compiler:
            raise ValueError("cl.exe is not available after setup-msvc")
        base, identity = cache_identity(env, lock, compiler)
        event = json.loads(pathlib.Path(env["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        keys = cache_keys(base, env["GITHUB_EVENT_NAME"], env["GITHUB_REF"], event.get("number"),
                          datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d"))
        executable = install(lock, env["RUNNER_TEMP"])
        if any(character.isspace() for character in str(executable) + str(PRELOAD)):
            raise ValueError("launcher/preload paths cannot contain shell whitespace")
        version = run(executable, "--version", env=env).strip()
        if version != "sccache " + lock["version"]:
            raise ValueError("sccache version differs from lock")
        state.update(ready=True, executable=str(executable), identity=identity, keys=keys,
                     budget=lock["cache_budget_bytes"][env["KLOGG_LABEL"]],
                     reason="verified executable ready; cache not activated")
        emit(env.get("GITHUB_OUTPUT"), {"ready": "true", **keys})
    except (OSError, ValueError, RuntimeError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        state["reason"] = str(error)
        emit(env.get("GITHUB_OUTPUT"), {"ready": "false"})
    (workspace / STATE_NAME).write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    print("Windows object cache: " + state["reason"])
    return state


def server_environment(state, workspace, env):
    return {**env, "SCCACHE_DIR": str(workspace / ".sccache"),
            "SCCACHE_CACHE_SIZE": str(state["budget"]), "SCCACHE_IDLE_TIMEOUT": "0",
            "SCCACHE_IGNORE_SERVER_IO_ERROR": "1"}


def activate(workspace, env):
    state = {"enabled": False, "ready": False, "run": run_identity(env)}
    try:
        loaded = json.loads((workspace / STATE_NAME).read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("invalid Windows cache activation state")
        state.update(loaded)
        if not state["ready"] or state["run"] != run_identity(env):
            raise ValueError("no current-run verified executable")
        server_env = server_environment(state, workspace, env)
        executable = state["executable"]
        # A fresh server plus a mandatory successful reset avoids lifetime totals.
        try:
            run(executable, "--stop-server", env=server_env)
        except RuntimeError:
            pass  # No existing server is the normal hosted-runner cold start.
        run(executable, "--start-server", env=server_env)
        run(executable, "--zero-stats", env=server_env)
        options = (env["KLOGG_CMAKE_OPTS"] + f" -C {PRELOAD.as_posix()}"
                   + f" -DCMAKE_C_COMPILER_LAUNCHER={pathlib.Path(executable).as_posix()}"
                   + f" -DCMAKE_CXX_COMPILER_LAUNCHER={pathlib.Path(executable).as_posix()}")
        emit(env.get("GITHUB_ENV"), {key: value for key, value in server_env.items() if key.startswith("SCCACHE_")})
        emit(env.get("GITHUB_ENV"), {"KLOGG_CMAKE_OPTS": options, "KLOGG_WINDOWS_OBJECT_CACHE": "measured",
                                   "KLOGG_OBJECT_CACHE_TOOL": "sccache"})
        with pathlib.Path(env["GITHUB_PATH"]).open("a", encoding="utf-8") as stream:
            stream.write(str(pathlib.Path(executable).parent) + "\n")
        state.update(enabled=True, reset=True, reason="active; statistics reset for this run")
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        state.update(enabled=False, reason="uncached compiler fallback: " + str(error))
    (workspace / STATE_NAME).write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    print("Windows object cache: " + state["reason"])
    return state


def normalized_stats(raw):
    stats = raw["stats"]
    def count(value):
        if type(value) is not int or value < 0:
            raise ValueError("invalid sccache counter")
        return value
    def languages(key):
        return sum(count(value) for value in stats[key]["counts"].values())
    hits, misses = languages("cache_hits"), languages("cache_misses")
    reasons = {key: count(value) for key, value in stats["not_cached"].items()}
    return {"requests": count(stats["compile_requests"]), "hits": hits, "misses": misses,
            "hit_rate": hits / (hits + misses) if hits + misses else None,
            "non_cacheable_reasons": reasons,
            "errors": languages("cache_errors") + count(stats["cache_write_errors"])
                      + count(stats["cache_read_errors"]) + count(stats["cache_timeouts"])}


def validate_compile_flags(build, executable):
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    for language in ("C", "CXX"):
        wanted = pathlib.Path(executable).as_posix()
        if not re.search(rf"^CMAKE_{language}_COMPILER_LAUNCHER:[^=]+={re.escape(wanted)}$", cache, re.MULTILINE):
            raise ValueError("configured compiler launcher missing")
    database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    if not isinstance(database, list) or not database:
        raise ValueError("missing compile database")
    checked = 0
    for entry in database:
        # ASM/RC do not use the C/C++ launchers or debug flags.
        if pathlib.Path(entry["file"]).suffix.lower() in {".asm", ".s", ".rc"}:
            continue
        command = entry.get("command", "")
        if not isinstance(command, str) or not command:
            raise ValueError("missing compile command")
        # Only inspect flag tokens: quoted source paths/definitions containing
        # '/Z7' are not flags. Backslash spelling of paths is immaterial here.
        tokens = shlex.split(command)
        if "/Z7" not in tokens or {"/Zi", "/ZI"}.intersection(tokens):
            raise ValueError("compile database contains non-cacheable MSVC debug flags")
        checked += 1
    if not checked:
        raise ValueError("compile database contains no C/C++ commands")


def bound_cache(directory, budget):
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("object cache directory missing or unsafe")
    files = []
    for path in [directory, *directory.rglob("*")]:
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("object cache contains a link/reparse point")
        if stat.S_ISREG(info.st_mode):
            files.append((info.st_mtime_ns, str(path), info.st_size))
        elif not stat.S_ISDIR(info.st_mode):
            raise ValueError("object cache contains a non-regular file")
    total = sum(size for _, _, size in files)
    removed = 0
    # sccache has stopped; enforce the save budget even if its asynchronous LRU
    # cleanup had not finished. No build directories or executables are saved.
    for _, path, size in sorted(files):
        if total <= budget:
            break
        pathlib.Path(path).unlink()
        total -= size
        removed += 1
    if not total:
        raise ValueError("object cache contains no reusable objects")
    return {"bytes": total, "budget_bytes": budget, "pruned_files": removed}


def report(workspace, env):
    result = {"schema_version": 1, "run": run_identity(env), "validated": False,
              "measurement_scope": "since successful pre-configure zero-stats", "targets_measured": False,
              "restore": {key: env.get(key, "") for key in ("PRIMARY_CACHE_MATCH", "MASTER_CACHE_MATCH",
                                                            "PRIMARY_CACHE_OUTCOME", "MASTER_CACHE_OUTCOME")}}
    try:
        state = json.loads((workspace / STATE_NAME).read_text(encoding="utf-8"))
        result["activation"] = state
        if state["run"] != run_identity(env) or not state["enabled"] or not state.get("reset"):
            raise ValueError(state.get("reason", "no current-run active cache"))
        server_env = server_environment(state, workspace, env)
        try:
            result["raw"] = json.loads(run(state["executable"], "--show-stats", "--stats-format=json", env=server_env))
        finally:
            run(state["executable"], "--stop-server", env=server_env)
        result["stats"] = normalized_stats(result["raw"])
        if result["stats"]["errors"] or not result["stats"]["requests"]:
            raise ValueError("cache errors or no compiler requests; refuse cache save")
        validate_compile_flags(workspace / env.get("KLOGG_BUILD_ROOT", "build_root"), state["executable"])
        result["storage"] = bound_cache(workspace / ".sccache", state["budget"])
        result.update(validated=True, reason="cache statistics, compiler flags and byte bound validated")
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, AttributeError, subprocess.SubprocessError) as error:
        result["reason"] = str(error)
    (workspace / REPORT_NAME).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    emit(env.get("GITHUB_OUTPUT"), {"validated": str(result["validated"]).lower()})
    print(json.dumps(result, indent=2))
    if env.get("GITHUB_STEP_SUMMARY"):
        with pathlib.Path(env["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
            stream.write("\n### Windows object cache\n\n```json\n" + json.dumps(result, indent=2) + "\n```\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "activate", "report"))
    args = parser.parse_args()
    workspace = pathlib.Path(os.environ["GITHUB_WORKSPACE"])
    {"prepare": prepare, "activate": activate, "report": report}[args.phase](workspace, dict(os.environ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
