"""Windows object-cache workflow contracts and offline behavior; no hosted runs."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("windows_cache_quality", ROOT / "scripts/lint_ci_quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALITY)


def windows_steps():
    blocks = QUALITY.workflow_job_steps((ROOT / ".github/workflows/ci-build.yml").read_text())["WindowsPackages"]
    return [dict(fields, **children) for fields, children in map(QUALITY.workflow_step_fields, blocks)]


class WindowsObjectCacheWorkflowTest(unittest.TestCase):
    def step(self, identifier):
        found = [step for step in windows_steps() if step.get("id") == identifier]
        self.assertEqual(len(found), 1, f"expected Windows cache step {identifier}")
        return found[0]

    def test_cache_is_optional_and_restores_only_exact_toolchain_namespaces(self):
        for identifier, prefix in (("restore-windows-cache", "primary"),
                                   ("restore-windows-master-cache", "master")):
            with self.subTest(identifier=identifier):
                step = self.step(identifier)
                self.assertIn("actions/cache/restore@", step["uses"])
                self.assertEqual(step["continue-on-error"], "true")
                self.assertEqual(step["with"]["path"], "${{ github.workspace }}/.sccache")
                self.assertEqual(step["with"]["restore-keys"],
                                 "${{ steps.windows-cache.outputs." + prefix + "-prefix }}")
        fallback = self.step("restore-windows-master-cache")
        self.assertIn("steps.restore-windows-cache.outputs.cache-matched-key == ''", fallback["if"])
        self.assertIn("github.event_name == 'pull_request'", fallback["if"])

    def test_stats_always_run_and_validated_save_precedes_package_upload(self):
        stats = self.step("windows-cache-report")
        self.assertIn("always()", stats["if"])
        save = self.step("save-windows-cache")
        self.assertIn("actions/cache/save@", save["uses"])
        for guard in ("success()", "steps.run-tests.outcome == 'success'",
                      "steps.windows-cache-report.outputs.validated == 'true'",
                      "steps.windows-cache.outputs.save-key != ''"):
            self.assertIn(guard, save["if"])
        self.assertEqual(save["with"]["path"], "${{ github.workspace }}/.sccache")
        steps = windows_steps()
        position = steps.index(save)
        ssl = next(i for i, step in enumerate(steps) if step.get("name") == "Verify SSL/TLS packaging")
        uploads = [i for i, step in enumerate(steps) if "actions/upload-artifact@" in step.get("uses", "")
                   and step.get("with", {}).get("name", "").startswith(("packages-", "symbols-"))]
        self.assertLess(ssl, position)
        self.assertTrue(uploads)
        self.assertTrue(all(position < upload for upload in uploads))

    def test_activation_is_after_restore_and_msvc_before_build(self):
        steps = windows_steps()
        prepare = steps.index(self.step("windows-cache"))
        activate = steps.index(self.step("activate-windows-cache"))
        dev_cmd = next(i for i, step in enumerate(steps) if step.get("uses") == "./.github/actions/setup-msvc")
        build = next(i for i, step in enumerate(steps) if step.get("uses") == "./.github/actions/agent-build")
        self.assertLess(dev_cmd, prepare)
        self.assertLess(prepare, steps.index(self.step("restore-windows-cache")))
        self.assertLess(steps.index(self.step("restore-windows-master-cache")), activate)
        self.assertLess(activate, build)
        self.assertIn("python3 -I", self.step("windows-cache")["run"])
        self.assertIn("python3 -I", self.step("activate-windows-cache")["run"])


CACHE_SPEC = importlib.util.spec_from_file_location("windows_cache", ROOT / "scripts/ci_windows_object_cache.py")
CACHE = importlib.util.module_from_spec(CACHE_SPEC)
CACHE_SPEC.loader.exec_module(CACHE)


def stats():
    return {"stats": {"compile_requests": 10, "cache_hits": {"counts": {"C/C++": 6}, "adv_counts": {"c++": 6}},
                      "cache_misses": {"counts": {"C/C++": 2}}, "cache_errors": {"counts": {}},
                      "cache_write_errors": 0, "cache_read_errors": 0, "cache_timeouts": 0,
                      "not_cached": {"not a compilation": 2}}}


class WindowsObjectCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.compiler = self.root / "cl.exe"
        self.compiler.write_bytes(b"compiler identity")
        self.executable = self.root / "sccache.exe"
        self.executable.write_bytes(b"MZcache")
        self.env = {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2", "GITHUB_SHA": "a" * 40,
                    "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/42/merge",
                    "RUNNER_TEMP": str(self.root), "KLOGG_CONFIG_ARCH": "x64", "KLOGG_QT_VERSION": "6.9.3",
                    "KLOGG_QT_ARCH": "win64_msvc2022_64", "KLOGG_LABEL": "x64-qt6",
                    "KLOGG_CMAKE_OPTS": "-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKLOGG_USE_LTO=ON",
                    "VCToolsVersion": "14.44.35207", "WindowsSDKVersion": "10.0.26100.0",
                    "VSCMD_ARG_TGT_ARCH": "x64", "KLOGG_SANITIZER": ""}
        for key in ("GITHUB_ENV", "GITHUB_OUTPUT", "GITHUB_PATH", "GITHUB_STEP_SUMMARY", "GITHUB_EVENT_PATH"):
            path = self.root / key
            path.write_text('{"number":42}' if key == "GITHUB_EVENT_PATH" else "")
            self.env[key] = str(path)
        self.lock = CACHE.load_lock()

    def state(self, **changes):
        state = {"enabled": True, "ready": True, "reset": True,
                 "run": CACHE.run_identity(self.env), "executable": str(self.executable),
                 "budget": 100, "reason": "active"}
        state.update(changes)
        (self.root / CACHE.STATE_NAME).write_text(json.dumps(state))
        return state

    def build(self, command='"C:/VS/cl.exe" /Z7 /O2 /c source.cpp'):
        build = self.root / "build_root"
        build.mkdir(exist_ok=True)
        (build / "CMakeCache.txt").write_text("".join(
            f"CMAKE_{language}_COMPILER_LAUNCHER:UNINITIALIZED={self.executable.as_posix()}\n"
            for language in ("C", "CXX")))
        (build / "compile_commands.json").write_text(json.dumps([
            {"file": "source.cpp", "command": command}]))
        return build

    def test_identity_separates_compiler_sdk_arch_qt_configuration_and_sanitizer(self):
        original = CACHE.cache_identity(self.env, self.lock, self.compiler)[0]
        for key, value in {"WindowsSDKVersion": "new-sdk", "VCToolsVersion": "new-tools",
                           "KLOGG_QT_VERSION": "6.10.0", "KLOGG_QT_ARCH": "other-qt",
                           "KLOGG_SANITIZER": "address", "KLOGG_LABEL": "x64-qt6-asan",
                           "KLOGG_CMAKE_OPTS": self.env["KLOGG_CMAKE_OPTS"] + " -DKLOGG_USE_LTO=OFF"}.items():
            with self.subTest(key=key):
                changed = {**self.env, key: value}
                self.assertNotEqual(CACHE.cache_identity(changed, self.lock, self.compiler)[0], original)
        changed = {**self.env, "KLOGG_CONFIG_ARCH": "x86", "VSCMD_ARG_TGT_ARCH": "x86"}
        self.assertNotEqual(CACHE.cache_identity(changed, self.lock, self.compiler)[0], original)
        self.compiler.write_bytes(b"new compiler")
        self.assertNotEqual(CACHE.cache_identity(self.env, self.lock, self.compiler)[0], original)
        with self.assertRaisesRegex(ValueError, "RelWithDebInfo"):
            CACHE.cache_identity({**self.env, "KLOGG_CMAKE_OPTS": "-DCMAKE_BUILD_TYPE=Debug"}, self.lock, self.compiler)
        with self.assertRaisesRegex(ValueError, "mismatch"):
            CACHE.cache_identity({**self.env, "VSCMD_ARG_TGT_ARCH": "x86"}, self.lock, self.compiler)

    def test_event_namespaces_cannot_write_master_from_pr_or_dispatch(self):
        day = "20261017"
        master = CACHE.cache_keys("base", "push", "refs/heads/master", None, day)
        pr = CACHE.cache_keys("base", "pull_request", "refs/pull/42/merge", 42, day)
        other = CACHE.cache_keys("base", "pull_request", "refs/pull/43/merge", 43, day)
        self.assertEqual(master["save-key"], "base-master-20261017")
        self.assertEqual(pr["save-key"], "base-pr-42-20261017")
        self.assertEqual(pr["master-prefix"], master["primary-prefix"])
        self.assertNotEqual(pr["primary-prefix"], other["primary-prefix"])
        for event, ref in (("workflow_dispatch", "refs/heads/master"), ("workflow_dispatch", "refs/heads/topic"),
                           ("push", "refs/heads/topic")):
            self.assertEqual(CACHE.cache_keys("base", event, ref, None, day)["save-key"], "")
        for number in (None, "", "42-master", "42\nmaster", -1, True):
            with self.assertRaises(ValueError):
                CACHE.cache_keys("base", "pull_request", "refs/pull/42/merge", number, day)
        with self.assertRaises(ValueError):
            CACHE.cache_keys("base", "pull_request_target", "refs/heads/master", 42, day)

    def test_stats_use_language_totals_not_duplicate_details_and_preserve_reasons(self):
        result = CACHE.normalized_stats(stats())
        self.assertEqual(result["hits"], 6)
        self.assertEqual(result["misses"], 2)
        self.assertEqual(result["hit_rate"], .75)
        self.assertEqual(result["non_cacheable_reasons"], {"not a compilation": 2})
        for value in (-1, True, "6"):
            raw = stats()
            raw["stats"]["cache_hits"]["counts"]["C/C++"] = value
            with self.assertRaises(ValueError):
                CACHE.normalized_stats(raw)

    def test_debug_flags_reject_shared_pdb_missing_debug_and_quoted_spoof(self):
        CACHE.validate_compile_flags(self.build(), str(self.executable))
        for command in ('cl.exe /Zi /O2 /c source.cpp', 'cl.exe /ZI /Z7 /c source.cpp',
                        'cl.exe /c source.cpp', 'cl.exe /c "a /Z7 source.cpp"',
                        'cl.exe /Z7 /c source.cpp /Zi'):
            with self.subTest(command=command), self.assertRaises(ValueError):
                CACHE.validate_compile_flags(self.build(command), str(self.executable))

    def test_debug_validation_checks_c_and_cpp_but_not_assembler(self):
        build = self.build()
        database = [{"file": "source.cpp", "command": 'cl.exe /Z7 /c source.cpp'},
                    {"file": "source.asm", "command": 'ml64.exe /c source.asm'}]
        (build / "compile_commands.json").write_text(json.dumps(database))
        CACHE.validate_compile_flags(build, str(self.executable))
        database[0] = {"file": "source.c", "command": 'cl.exe /Zi /c source.c'}
        (build / "compile_commands.json").write_text(json.dumps(database))
        with self.assertRaises(ValueError):
            CACHE.validate_compile_flags(build, str(self.executable))

    def test_pinned_download_materializes_only_verified_member(self):
        payload = io.BytesIO()
        with zipfile.ZipFile(payload, "w") as archive:
            archive.writestr(self.lock["member"], b"MZfixture")
            archive.writestr("../json.py", "raise RuntimeError('untrusted')")
        data = payload.getvalue()
        lock = {**self.lock, "archive_sha256": hashlib.sha256(data).hexdigest()}
        with mock.patch.object(CACHE.urllib.request, "urlopen", return_value=io.BytesIO(data)):
            executable = CACHE.install(lock, self.root)
        self.assertEqual(executable.read_bytes(), b"MZfixture")
        self.assertFalse((self.root / "json.py").exists())
        with mock.patch.object(CACHE.urllib.request, "urlopen", return_value=io.BytesIO(data)):
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                CACHE.install({**lock, "archive_sha256": "0" * 64}, self.root)
        with mock.patch.object(CACHE.urllib.request, "urlopen", return_value=io.BytesIO(data)), \
                mock.patch.object(CACHE, "MAX_ARCHIVE_BYTES", 10):
            with self.assertRaisesRegex(ValueError, "byte limit"):
                CACHE.install(lock, self.root)

    def test_prepare_download_failure_preserves_uncached_build_and_reports_reason(self):
        with mock.patch.object(CACHE, "NATIVE_WINDOWS", True), \
                mock.patch.object(CACHE.shutil, "which", return_value=str(self.compiler)), \
                mock.patch.object(CACHE, "install", side_effect=OSError("offline")):
            state = CACHE.prepare(self.root, self.env)
        self.assertFalse(state["ready"])
        self.assertEqual(state["reason"], "offline")
        self.assertNotIn("save-key", pathlib.Path(self.env["GITHUB_OUTPUT"]).read_text())
        self.assertEqual(pathlib.Path(self.env["GITHUB_ENV"]).read_text(), "KLOGG_WINDOWS_OBJECT_CACHE=disabled\n")
        with mock.patch.object(CACHE, "run") as runner:
            result = CACHE.report(self.root, self.env)
        runner.assert_not_called()
        self.assertFalse(result["validated"])
        self.assertEqual(result["reason"], "offline")

    def test_prepare_publishes_only_verified_executable_and_namespace(self):
        with mock.patch.object(CACHE, "NATIVE_WINDOWS", True), \
                mock.patch.object(CACHE.shutil, "which", return_value=str(self.compiler)), \
                mock.patch.object(CACHE, "install", return_value=self.executable), \
                mock.patch.object(CACHE, "run", return_value="sccache " + self.lock["version"] + "\n"):
            state = CACHE.prepare(self.root, self.env)
        self.assertTrue(state["ready"])
        self.assertFalse(state["enabled"])
        self.assertIn("-pr-42-", state["keys"]["save-key"])
        self.assertEqual(state["budget"], self.lock["cache_budget_bytes"]["x64-qt6"])
        self.assertNotIn("COMPILER_LAUNCHER", pathlib.Path(self.env["GITHUB_ENV"]).read_text())

    def test_malformed_lock_is_rejected_before_any_download(self):
        path = self.root / "lock.json"
        for value in ([], {}, {**self.lock, "archive_url": "https://evil.invalid/tool.zip"},
                      {**self.lock, "member": "../sccache.exe"}, {**self.lock, "archive_sha256": "latest"},
                      {**self.lock, "cache_budget_bytes": {"x64-qt6": True}}):
            with self.subTest(value=value):
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    CACHE.load_lock(path)

    def test_activation_fallback_never_leaves_launcher_options(self):
        self.state(enabled=False)
        with mock.patch.object(CACHE, "run", side_effect=RuntimeError("server unavailable")):
            state = CACHE.activate(self.root, self.env)
        self.assertFalse(state["enabled"])
        self.assertIn("uncached compiler fallback", state["reason"])
        self.assertNotIn("COMPILER_LAUNCHER", pathlib.Path(self.env["GITHUB_ENV"]).read_text())

    def test_activation_lost_or_malformed_state_falls_back_without_launching(self):
        for contents in (None, "not-json", "[]"):
            with self.subTest(contents=contents):
                path = self.root / CACHE.STATE_NAME
                path.unlink(missing_ok=True)
                if contents is not None:
                    path.write_text(contents)
                with mock.patch.object(CACHE, "run") as runner:
                    result = CACHE.activate(self.root, self.env)
                self.assertFalse(result["enabled"])
                runner.assert_not_called()

    def test_activation_resets_stats_before_enabling_both_launchers(self):
        self.state(enabled=False)
        with mock.patch.object(CACHE, "run", return_value="") as runner:
            state = CACHE.activate(self.root, self.env)
        self.assertTrue(state["enabled"])
        self.assertEqual([call.args[1] for call in runner.call_args_list],
                         ["--stop-server", "--start-server", "--zero-stats"])
        exported = pathlib.Path(self.env["GITHUB_ENV"]).read_text()
        for value in ("CMAKE_C_COMPILER_LAUNCHER=", "CMAKE_CXX_COMPILER_LAUNCHER=", "WindowsObjectCache.cmake",
                      "SCCACHE_IGNORE_SERVER_IO_ERROR=1", "KLOGG_OBJECT_CACHE_TOOL=sccache"):
            self.assertIn(value, exported)
        self.assertNotIn("KLOGG_USE_LTO=OFF", exported)

    def test_report_binds_current_run_stops_server_and_bounds_saved_objects(self):
        self.state()
        self.build()
        cache = self.root / ".sccache"
        cache.mkdir()
        for name in ("older", "newer"):
            (cache / name).write_bytes(b"x" * 60)
        os.utime(cache / "older", (1, 1))
        with mock.patch.object(CACHE, "run", side_effect=[json.dumps(stats()), ""]) as runner:
            result = CACHE.report(self.root, self.env)
        self.assertTrue(result["validated"], result)
        self.assertEqual(result["run"]["GITHUB_RUN_ATTEMPT"], "2")
        self.assertEqual(result["storage"]["bytes"], 60)
        self.assertFalse((cache / "older").exists())
        self.assertTrue((cache / "newer").exists())
        self.assertEqual(runner.call_args_list[-1].args[1], "--stop-server")
        self.assertIn("validated=true", pathlib.Path(self.env["GITHUB_OUTPUT"]).read_text())

    def test_report_never_saves_disabled_stale_invalid_or_erroring_cache(self):
        self.build()
        for scenario in ("disabled", "stale", "bad-json", "missing-counter", "cache-error"):
            with self.subTest(scenario=scenario):
                state = self.state(enabled=scenario != "disabled")
                if scenario == "stale":
                    self.state(run={**state["run"], "GITHUB_RUN_ATTEMPT": "1"})
                raw = stats()
                if scenario == "missing-counter":
                    del raw["stats"]["cache_hits"]
                if scenario == "cache-error":
                    raw["stats"]["cache_write_errors"] = 1
                output = "invalid" if scenario == "bad-json" else json.dumps(raw)
                with mock.patch.object(CACHE, "run", side_effect=[output, ""]):
                    self.assertFalse(CACHE.report(self.root, self.env)["validated"])

    def test_cache_links_fail_closed_without_deleting_link_target(self):
        cache = self.root / ".sccache"
        cache.mkdir()
        (cache / "link").symlink_to(self.compiler)
        with self.assertRaisesRegex(ValueError, "link/reparse"):
            CACHE.bound_cache(cache, 1)
        self.assertEqual(self.compiler.read_bytes(), b"compiler identity")

    def test_preload_preserves_real_msvc_crt_toolchain_symbols_and_lto_defaults(self):
        # Exercise CMake's actual platform initialization, not a hand-written
        # approximation: the legacy baseline puts /MD and /MDd in config flags.
        for policy in ("OLD", "NEW"):
            with self.subTest(policy=policy), tempfile.TemporaryDirectory() as directory:
                script = pathlib.Path(directory) / "probe.cmake"
                script.write_text(f'''cmake_minimum_required(VERSION 3.14)
cmake_policy(SET CMP0091 {policy})
cmake_policy(SET CMP0141 {policy})
set(CMAKE_GENERATOR Ninja)
set(CMAKE_SYSTEM_NAME Windows)
foreach(lang C CXX)
  set(CMAKE_${{lang}}_COMPILER_ID MSVC)
  set(CMAKE_${{lang}}_COMPILER_VERSION 19.44)
  set(MSVC_${{lang}}_ARCHITECTURE_ID x64)
  foreach(config RELWITHDEBINFO DEBUG)
    set(CMAKE_${{lang}}_FLAGS_${{config}}_INIT "/DUSER_TOOLCHAIN=1")
  endforeach()
endforeach()
include("{CACHE.PRELOAD.as_posix()}")
include(Platform/Windows-MSVC)
# No compiler/build is needed for the flags probe; suppress only RC discovery.
macro(__windows_compiler_msvc_enable_rc flags)
endmacro()
set(original_linker "${{CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO_INIT}}")
set(original_runtime "${{CMAKE_MSVC_RUNTIME_LIBRARY_DEFAULT}}")
foreach(lang C CXX)
  __windows_compiler_msvc(${{lang}})
  foreach(config RELWITHDEBINFO DEBUG)
    string(REPLACE "/Zi" "/Z7" expected_${{config}} "${{CMAKE_${{lang}}_FLAGS_${{config}}_INIT}}")
    string(STRIP "${{expected_${{config}}}}" expected_${{config}})
  endforeach()
  if(DEFINED CMAKE_USER_MAKE_RULES_OVERRIDE)
    include("${{CMAKE_USER_MAKE_RULES_OVERRIDE}}")
  endif()
  foreach(config RELWITHDEBINFO DEBUG)
    string(STRIP "${{CMAKE_${{lang}}_FLAGS_${{config}}_INIT}}" initialized)
    set(CMAKE_${{lang}}_FLAGS_${{config}} "${{initialized}}" CACHE STRING "compiler flags")
    if(NOT CMAKE_${{lang}}_FLAGS_${{config}} STREQUAL expected_${{config}})
      message(FATAL_ERROR "${{lang}} ${{config}} lost MSVC/toolchain defaults: '${{CMAKE_${{lang}}_FLAGS_${{config}}}}' expected '${{expected_${{config}}}}'")
    endif()
  endforeach()
  if(NOT CMAKE_${{lang}}_COMPILE_OPTIONS_IPO STREQUAL "/GL")
    message(FATAL_ERROR "LTO changed")
  endif()
endforeach()
if(NOT CMAKE_MSVC_DEBUG_INFORMATION_FORMAT STREQUAL "Embedded")
  message(FATAL_ERROR "new-policy dependencies must also embed debug info")
endif()
if(NOT CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO_INIT STREQUAL original_linker
   OR NOT CMAKE_MSVC_RUNTIME_LIBRARY_DEFAULT STREQUAL original_runtime
   OR DEFINED KLOGG_USE_LTO)
  message(FATAL_ERROR "preload changed linker PDB, runtime or LTO settings")
endif()
''')
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
