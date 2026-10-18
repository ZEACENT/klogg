"""Compiler-cache budgets and success-only save ordering."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest

from test_ci_environment_workflow import QUALITY as LINT, event_expression

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/ci_ccache.py"
WORKFLOW = ROOT / ".github/workflows/ci-build.yml"


def load_module():
    spec = importlib.util.spec_from_file_location("ci_ccache", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rows(text, job):
    return [LINT.workflow_step_fields(step)
            for step in LINT.workflow_job_steps(text)[job]]


def lifecycle(text, job, event, failed_stage=None, own_pr=True):
    """Project actual step predicates with Actions' implicit success() gating."""
    successful = True
    saves = []
    for fields, children in rows(text, job):
        name = fields.get("name", "")
        uses = fields.get("uses", "")
        stage = None
        if uses.endswith(("/agent-build", "/docker-build")):
            stage = "build"
        elif uses.endswith(("/agent-run-tests", "/docker-run-tests")):
            stage = "test"
        elif uses.endswith(("/agent-package-mac", "/docker-package")):
            stage = "package"
        elif children.get("with", {}).get("name", "").startswith(("packages-", "symbols-")):
            stage = "upload"
        elif uses.startswith("actions/cache/save@"):
            stage = "save"
        if not stage:
            continue
        condition = fields.get("if", "${{ True }}")
        condition = (condition.replace("success()", repr(successful))
                     .replace("env.KLOGG_SANITIZER", "''")
                     .replace("env.KLOGG_PACKAGE_ENABLED", "'true'")
                     .replace("steps.restore-pr-ccache.outcome", repr("success" if own_pr else "skipped"))
                     .replace("inputs.qualification-mode", "'release'"))
        if not successful or not event_expression(condition, event, "off"):
            continue
        if stage == "save":
            saves.append(name)
        if stage == failed_stage:
            successful = False
    return successful, saves


class CacheSaveLifecycleTest(unittest.TestCase):
    def test_upload_failure_does_not_discard_validated_cache(self):
        text = WORKFLOW.read_text()
        for job in ("LinuxPackages", "MacPackages", "MacArmPackages"):
            with self.subTest(job=job):
                successful, saves = lifecycle(text, job, "push", "upload")
                self.assertFalse(successful, "upload must still fail the job and required gate")
                self.assertEqual(len(saves), 1, "save validated cache before publication transport")

    def test_failed_build_test_or_package_never_saves(self):
        text = WORKFLOW.read_text()
        for job in ("LinuxPackages", "MacPackages", "MacArmPackages"):
            for stage in ("build", "test", "package"):
                with self.subTest(job=job, stage=stage):
                    self.assertEqual(lifecycle(text, job, "push", stage), (False, []))

    def test_event_write_boundaries(self):
        text = WORKFLOW.read_text()
        for job in ("LinuxPackages", "MacPackages"):
            for event in ("push", "pull_request", "workflow_dispatch", "schedule"):
                for own_pr in (True, False):
                    with self.subTest(job=job, event=event, own_pr=own_pr):
                        _, saves = lifecycle(text, job, event, own_pr=own_pr)
                        expected = event == "push" or (job == "MacPackages" and event == "pull_request" and own_pr)
                        self.assertEqual(len(saves), int(expected))

    def test_lint_rejects_late_unconditional_or_spoofed_saves(self):
        text = WORKFLOW.read_text()
        self.assertEqual(LINT.ccache_workflow_issues(text), [])
        start = text.index("      - name: Bound macOS ccache before save")
        end = text.index("      - uses: ./.github/actions/upload-release-artifact", start)
        block = text[start:end]
        bad = text[:start] + text[end:]
        boundary = bad.index("\n  MacArmPackages:")
        late = bad[:boundary] + block + bad[boundary:]
        for mutation in (late, late + "\n# " + block.replace("\n", "\n# "),
                         text.replace("success() && github.event_name == 'push'", "always() && github.event_name == 'push'"),
                         text.replace("python3 scripts/ci_ccache.py trim", "printf 'python3 scripts/ci_ccache.py trim'")):
            with self.subTest(mutation=mutation[:80]):
                self.assertTrue(LINT.ccache_workflow_issues(mutation))
        self.assertTrue(LINT.ccache_workflow_issues("jobs: invalid"))


class CcacheBudgetTest(unittest.TestCase):
    def test_prepare_overrides_inherited_budget_for_all_known_labels(self):
        module = load_module()
        for label, size in module.BUDGETS.items():
            with self.subTest(label=label):
                values = module.environment(label)
                self.assertEqual(values["CCACHE_MAXSIZE"], size)
                expected_check = "mtime" if label.startswith("ubuntu-") else "content"
                self.assertEqual(values["CCACHE_COMPILERCHECK"], expected_check)
                self.assertEqual(values["CCACHE_COMPRESS"], "true")
        with self.assertRaises(ValueError):
            module.environment("unknown")

    @unittest.skipUnless(shutil.which("ccache"), "ccache executable unavailable")
    def test_trim_overrides_image_environment_and_reports_effective_size(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "cache"
            env = {**os.environ, "CCACHE_DIR": str(cache), "CCACHE_MAXSIZE": "2G"}
            result = subprocess.run([sys.executable, str(SCRIPT), "trim", "--label", "ubuntu-22.04-deb"],
                                    env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["budget_bytes"], 500_000_000)
            self.assertEqual(report["effective_max_size_bytes"], 500_000_000)
            self.assertLessEqual(report["payload_bytes"], report["budget_bytes"])
            self.assertIn("max_size", report["effective_config"])

    @unittest.skipUnless(shutil.which("ccache") and shutil.which("clang"), "ccache/clang unavailable")
    def test_real_objects_reuse_across_warm_builds_and_invalidate_changed_headers(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "probe.c"
            header = root / "value.h"
            source.write_text('#include "value.h"\nint main(void) { return VALUE; }\n')
            header.write_text("#define VALUE 0\n")
            for flags in ([], ["-fsanitize=address,undefined"]):
                with self.subTest(flags=flags):
                    cache = root / ("sanitized-cache" if flags else "cache")
                    env = {**os.environ, **module.environment("intel-qt6"),
                           "CCACHE_DIR": str(cache), "CCACHE_BASEDIR": str(root),
                           "ASAN_OPTIONS": "detect_leaks=0"}
                    obj = root / "probe.o"
                    binary = root / "probe"
                    command = ["ccache", shutil.which("clang"), *flags, "-g", "-c", str(source), "-o", str(obj)]
                    def run(args):
                        return subprocess.run(args, env=env, check=True, capture_output=True, text=True)
                    run(["ccache", "--zero-stats"])
                    for _ in range(4):
                        run(command)
                        run([shutil.which("clang"), *flags, str(obj), "-o", str(binary)])
                        run([str(binary)])
                        obj.unlink()
                    stats = dict(line.split() for line in run(["ccache", "--print-stats"]).stdout.splitlines())
                    hits = int(stats.get("direct_cache_hit", "0")) + int(stats.get("preprocessed_cache_hit", "0"))
                    self.assertEqual(hits, 3)
                    self.assertEqual(int(stats["cache_miss"]), 1)
                    header.write_text("#define VALUE 1\n")
                    run(command)
                    stats = dict(line.split() for line in run(["ccache", "--print-stats"]).stdout.splitlines())
                    self.assertEqual(int(stats["cache_miss"]), 2)
                    header.write_text("#define VALUE 0\n")

    @unittest.skipUnless(shutil.which("ccache") and shutil.which("clang"), "ccache/clang unavailable")
    def test_linux_policy_reuses_existing_immutable_toolchain_seed(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "probe.c"
            source.write_text("int main(void) { return 0; }\n")
            obj = root / "probe.o"
            command = ["ccache", shutil.which("clang"), "-c", str(source), "-o", str(obj)]
            base = {**os.environ, "CCACHE_DIR": str(root / "cache"), "CCACHE_BASEDIR": str(root)}

            def compile_with(settings):
                env = {**base, **settings}
                subprocess.run(command, env=env, check=True, capture_output=True, text=True)
                obj.unlink()
                stats = subprocess.run(["ccache", "--print-stats"], env=env, check=True,
                                       capture_output=True, text=True).stdout
                return {key: int(value) for key, value in (line.split() for line in stats.splitlines())}

            # Existing Linux master entries use ccache's mtime default. A PR is
            # restore-only and cannot replace that seed with a new hash policy.
            seeded = compile_with({"CCACHE_COMPILERCHECK": "mtime"})
            self.assertEqual(seeded["cache_miss"], 1)
            reused = compile_with(module.environment("ubuntu-22.04-deb"))
            self.assertEqual(reused["cache_miss"], 1, "unchanged qualified compiler seed must remain reusable")
            self.assertEqual(reused.get("direct_cache_hit", 0) + reused.get("preprocessed_cache_hit", 0), 1)
            changed = compile_with({"CCACHE_COMPILERCHECK": "content"})
            self.assertEqual(changed["cache_miss"], 2, "changing compiler identity policy alone invalidates objects")

    def test_size_formats_and_unknown_units_fail_closed(self):
        module = load_module()
        for value, expected in (("250M", 250_000_000), ("250.0 MB", 250_000_000),
                                ("1.0 GiB", 1_073_741_824), ("1024", 1024)):
            self.assertEqual(module.size_bytes(value), expected)
        for value in ("", "unlimited", "-1G", "2G; true", "NaNM"):
            with self.assertRaises(ValueError):
                module.size_bytes(value)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ("ccache", "clang", "cmake", "ninja")),
                         "native compiler-cache probe tools unavailable")
    def test_linux_launcher_survives_nested_shell_and_overrides_image_budget(self):
        module = load_module()
        fields = next(fields for fields, _ in rows(WORKFLOW.read_text(), "LinuxPackages")
                      if fields.get("name") == "Enable ccache compiler launcher")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            github_env = root / "github-env"
            env = {**os.environ, **module.environment("ubuntu-22.04-deb"),
                   "GITHUB_ENV": str(github_env), "KLOGG_CMAKE_OPTS": "-G Ninja"}
            subprocess.run(["bash", "-eu", "-c", fields["run"]], env=env, check=True, capture_output=True)
            options = github_env.read_text().strip().split("=", 1)[1]
            source = root / "probe.c"
            source.write_text("int main(void) { return 0; }\n")
            (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.16)\n"
                                                 "project(CacheProbe C CXX)\nadd_executable(probe probe.c)\n")
            log = root / "ccache.log"
            env.update(CCACHE_MAXSIZE="2G", CCACHE_DIR=str(root / "cache"), CCACHE_LOGFILE=str(log))
            # docker-build expands the options INTO a bash -lc command, not an
            # argument array. Exercise that second shell parse with real CMake.
            command = (f"cmake -S {shlex.quote(str(root))} -B {shlex.quote(str(root / 'build'))} "
                       f"{options} -DCMAKE_C_COMPILER={shlex.quote(shutil.which('clang'))}")
            configured = subprocess.run(["bash", "-eu", "-c", command], env=env,
                                        text=True, capture_output=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            built = subprocess.run(["cmake", "--build", str(root / "build"), "--verbose"],
                                   env=env, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            self.assertIn("CCACHE_MAXSIZE=500M", built.stdout)
            effective = [line for line in log.read_text().splitlines() if "max_size =" in line]
            self.assertTrue(effective)
            for line in effective:
                self.assertEqual(module.size_bytes(line.split("max_size =", 1)[1]), 500_000_000)


if __name__ == "__main__":
    unittest.main()
