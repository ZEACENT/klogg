"""Exercise wrapper scripts without building images or running real containers."""
import importlib.util
import os
import pathlib
import re
import shlex
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).parents[2]
ACTION = ROOT / ".github/actions/linux-validate/action.yml"
SPEC = importlib.util.spec_from_file_location("linux_validate_quality", ROOT / "scripts/lint_ci_quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(QUALITY)


def action_steps(text=None):
    lines = (ACTION.read_text() if text is None else text).splitlines()
    return QUALITY.workflow_step_blocks(lines[lines.index("runs:"):])


def run_body(step):
    for index, line in enumerate(step):
        if re.fullmatch(r"\s+run: \|", line):
            return textwrap.dedent("\n".join(step[index + 1:])).strip() + "\n"
    raise AssertionError("expected a literal run block")


class LinuxValidateActionTest(unittest.TestCase):
    def configuration(self, family="jammy-qt5", profile="asan-lsan", inherited=None, body=None):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            workspace = root / "host checkout with spaces"
            workspace.mkdir()
            output = root / "github-env"
            env = dict(os.environ, GITHUB_WORKSPACE=str(workspace), GITHUB_ENV=str(output),
                       KLOGG_INPUT_FAMILY=family, KLOGG_INPUT_PROFILE=profile,
                       PYTHONPATH=str(ROOT / "scripts"))
            env.update(inherited or {})
            result = subprocess.run(["bash", "-euo", "pipefail", "-c", body or run_body(action_steps()[0])],
                                    cwd=ROOT, env=env, check=False, capture_output=True, text=True)
            content = output.read_text() if output.exists() else ""
            values = dict(line.split("=", 1) for line in content.splitlines())
            return result, values, str(workspace)

    def test_profile_keeps_container_paths_but_bind_mount_uses_host_workspace(self):
        result, env, workspace = self.configuration()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(env.get("KLOGG_WORKSPACE"), workspace)
        self.assertIn("-DCPM_SOURCE_CACHE=/usr/local/cpm_cache", shlex.split(env["KLOGG_CMAKE_OPTS"]))
        self.assertIn("suppressions=/usr/local/tests/sanitizers/asan_suppressions.txt", env["ASAN_OPTIONS"])
        self.assertEqual(env["KLOGG_CI_IMAGE_PREFIX"] + env["KLOGG_CONTAINER_SUFFIX"], "zeacent/klogg_ubuntu22.04")

    def test_reuses_one_build_then_tests_and_optional_full_packaging(self):
        steps = [QUALITY.workflow_step_fields(step) for step in action_steps()]
        uses = [fields["uses"] for fields, _ in steps if "uses" in fields]
        self.assertEqual(uses, ["./.github/actions/docker-build", "./.github/actions/docker-run-tests",
                               "./.github/actions/docker-package"])
        package, children = next((fields, children) for fields, children in steps
                                 if fields.get("uses") == "./.github/actions/docker-package")
        self.assertEqual(package["if"], "${{ env.KLOGG_PACKAGE_ENABLED == 'true' }}")
        self.assertEqual(children["with"]["collect-symbols"], "true")
        self.assertEqual(children["with"]["check-command"], "${{ env.KLOGG_CHECK_COMMAND }}")

    def test_shared_compiler_actions_never_implicitly_pull_a_mutable_local_tag(self):
        for name in ("docker-build", "docker-run-tests", "docker-package"):
            text = (ROOT / ".github/actions" / name / "action.yml").read_text()
            for step in action_steps(text):
                body = "\n".join(step)
                if "docker run" in body and "KLOGG_CI_IMAGE_PREFIX" in body:
                    with self.subTest(action=name, step=QUALITY.workflow_step_fields(step)[0].get("name")):
                        self.assertEqual(body.count("docker run"), body.count("docker run --pull=never"))
        package = (ROOT / ".github/actions/docker-package/action.yml").read_text()
        check = next(step for step in action_steps(package)
                     if QUALITY.workflow_step_fields(step)[0].get("name") == "Check package")
        self.assertIn("${{ inputs.check-container }}", run_body(check))
        self.assertNotIn("--pull=never", run_body(check))

    def test_tsan_requires_durable_corresponding_sources_before_application_build(self):
        text = ACTION.read_text()
        steps = action_steps(text)
        source = next((step for step in steps if "verify_tsan_qt_sources.py" in "\n".join(step)), None)
        self.assertIsNotNone(source, "TSan qualification lacks a corresponding-source check")
        fields = QUALITY.workflow_step_fields(source)[0]
        self.assertEqual(fields["if"], "${{ env.KLOGG_SANITIZER == 'thread' }}")
        body = run_body(source)
        self.assertIn("--require-locked", body)
        self.assertIn("--sources /usr/share/klogg-ci/qt-sources", body)
        self.assertIn("--repo-root /usr/local", body)
        self.assertIn("--pull=never", body)
        self.assertIn("--network=none", body)
        self.assertLess(text.index("verify_tsan_qt_sources.py"), text.index("uses: ./.github/actions/docker-build"))

    def test_every_build_profile_matches_current_feature_and_runtime_contracts(self):
        cases = (
            ("focal-qt5-gcc13", "appimage", "_ubuntu20.04", "", True),
            ("jammy-qt5", "deb", "_ubuntu22.04", "", True),
            ("jammy-qt5", "asan-lsan", "_ubuntu22.04", "address", False),
            ("jammy-qt5", "ubsan", "_ubuntu22.04", "undefined", False),
            ("noble-qt6", "deb", "_ubuntu24.04", "", True),
            ("resolute-qt6", "deb", "_ubuntu26.04", "", True),
            ("jammy-qt5-tsan", "tsan", "_ubuntu22.04-tsan", "thread", False),
        )
        for family, profile, suffix, sanitizer, package in cases:
            with self.subTest(family=family, profile=profile):
                result, env, workspace = self.configuration(family, profile)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(env["KLOGG_WORKSPACE"], workspace)
                self.assertEqual(env["KLOGG_CONTAINER_SUFFIX"], suffix)
                self.assertEqual(env["KLOGG_SANITIZER"], sanitizer)
                self.assertEqual(env["KLOGG_PACKAGE_ENABLED"], str(package).lower())
                self.assertEqual(env["KLOGG_PACKAGE_TAG"], "vs-gen" if package else {"address": "asan", "undefined": "ubsan", "thread": "tsan"}[sanitizer])
                options = shlex.split(env["KLOGG_CMAKE_OPTS"])
                for option in ("-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DKLOGG_GENERIC_CPU=ON",
                               "-DWARNINGS_AS_ERRORS=ON", "-DCMAKE_INSTALL_PREFIX=/usr",
                               "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCPM_SOURCE_CACHE=/usr/local/cpm_cache"):
                    self.assertIn(option, options)
                self.assertEqual("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache" in options, sanitizer != "thread")
                self.assertEqual("-DKLOGG_ADB_HELPER_REQUIRED=ON" in options, package)
                if package:
                    self.assertIn("-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=/usr/local/prefetch_artifacts/adb-helper", options)
                if sanitizer:
                    self.assertIn("-DKLOGG_USE_LTO=OFF", options)
                    self.assertEqual(env["KLOGG_BUILD_ROOT"], "build_root")
                if sanitizer in ("address", "undefined"):
                    self.assertIn("-DKLOGG_USE_SENTRY=OFF", options)
                    self.assertIn("-DKLOGG_CI_LIGHT_DEBUG=ON", options)
                if sanitizer == "address":
                    self.assertIn("detect_leaks=1", env["ASAN_OPTIONS"])
                    self.assertIn("fast_unwind_on_malloc=0", env["LSAN_OPTIONS"])
                if sanitizer == "undefined":
                    self.assertEqual(env["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")
                if sanitizer == "thread":
                    self.assertIn("-DKLOGG_TSAN_QT_VERSION=5.15.19", options)
                    self.assertIn("-DCMAKE_CXX_COMPILER=clang++-14", options)
                    self.assertEqual(env["TSAN_OPTIONS"], "halt_on_error=1:external_symbolizer_path=/usr/bin/llvm-symbolizer-14")

    def test_analysis_unknown_and_cross_family_profiles_fail_without_partial_environment(self):
        for family, profile in (("noble-qt693-analysis", "static"), ("noble-qt693-analysis", "coverage"),
                                ("noble-qt693-analysis", "codeql"), ("unknown", "deb"),
                                ("noble-qt6", "asan-lsan"), ("jammy-qt5", "deb\nINJECTED=yes")):
            with self.subTest(family=family, profile=profile):
                result, env, _ = self.configuration(family, profile)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(env, {})

    def test_sanitizer_environment_is_scoped_not_inherited_from_previous_profile(self):
        inherited = {key: "stale-inherited-value" for key in ("ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")}
        result, env, _ = self.configuration(profile="ubsan", inherited=inherited)
        self.assertEqual(result.returncode, 0, result.stderr)
        for key in ("ASAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS"):
            self.assertEqual(env[key], "")
        self.assertEqual(env["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")

    def test_invalid_host_workspace_cannot_inject_github_environment(self):
        result, env, _ = self.configuration(inherited={"GITHUB_WORKSPACE": "/host/path\nINJECTED=yes"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(env, {})

    def named_step(self, name):
        return next(step for step in action_steps() if QUALITY.workflow_step_fields(step)[0].get("name") == name)

    def test_preflight_build_instrumentation_runtime_tests_and_package_are_ordered(self):
        fields = [QUALITY.workflow_step_fields(step)[0] for step in action_steps()]
        labels = [item.get("name", item.get("uses")) for item in fields]
        self.assertEqual(labels, [
            "Configure Linux qualification profile", "Require prepared local image", "Verify TSan image prerequisites",
            "Verify durable TSan Qt corresponding sources",
            "./.github/actions/docker-build", "Verify ASan-instrumented test binaries",
            "Verify UBSan-instrumented test binaries", "Verify TSan Qt runtime provenance",
            "./.github/actions/docker-run-tests", "./.github/actions/docker-package",
        ])
        for item in fields:
            self.assertNotIn("continue-on-error", item)
        for name, sanitizer in (("Verify ASan-instrumented test binaries", "address"),
                                ("Verify UBSan-instrumented test binaries", "undefined"),
                                ("Verify TSan image prerequisites", "thread"),
                                ("Verify TSan Qt runtime provenance", "thread")):
            item, _ = QUALITY.workflow_step_fields(self.named_step(name))
            self.assertEqual(item["if"], "${{ env.KLOGG_SANITIZER == '" + sanitizer + "' }}")

    def test_full_test_and_package_forwarding_matches_existing_linux_steps(self):
        original = []
        for block in QUALITY.workflow_job_blocks((ROOT / ".github/workflows/ci-build.yml").read_text()).values():
            original.extend(QUALITY.workflow_step_fields(step) for step in QUALITY.workflow_step_blocks(block))
        for action in ("docker-build", "docker-run-tests", "docker-package"):
            reference = "./.github/actions/" + action
            expected = next(children["with"] for fields, children in original if fields.get("uses") == reference)
            actual = next(children["with"] for fields, children in map(QUALITY.workflow_step_fields, action_steps())
                          if fields.get("uses") == reference)
            self.assertEqual(actual, expected)
        tests = (ROOT / ".github/actions/docker-run-tests/action.yml").read_text()
        self.assertIn("ctest --output-on-failure --parallel $(nproc)", tests)
        for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LSAN_OPTIONS"):
            self.assertIn("--env " + option, tests)
        self.assertIn("--env LANG=C.UTF-8", tests)

    def test_no_environment_acquisition_tokens_or_provenance_bypass(self):
        text = ACTION.read_text()
        for forbidden in ("docker pull", "docker build", "docker/build-push", "setup-buildx", "apt-get",
                          "GH_TOKEN", "GITHUB_TOKEN", "github.token", "secrets.", "continue-on-error",
                          "gh attestation", "--skip", "qualification-mode"):
            self.assertNotIn(forbidden, text)
        self.assertNotIn("inputs.image", text)
        inspect = run_body(self.named_step("Require prepared local image"))
        self.assertIn('docker image inspect "$KLOGG_CI_IMAGE_PREFIX$KLOGG_CONTAINER_SUFFIX"', inspect)
        self.assertNotIn("||", inspect)

    def test_missing_prepared_image_fails_without_any_pull_fallback(self):
        body = run_body(self.named_step("Require prepared local image"))
        # A shell function replaces Docker completely; no daemon is contacted.
        fixture = 'docker() { printf "called:%s\\n" "$*"; return 7; };\n'
        result = subprocess.run(["bash", "-euo", "pipefail", "-c", fixture + body + 'printf "unexpected-success\\n"'],
                                env=dict(os.environ, KLOGG_CI_IMAGE_PREFIX="zeacent/klogg",
                                         KLOGG_CONTAINER_SUFFIX="_ubuntu22.04"),
                                check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 7)
        self.assertNotIn("unexpected-success", result.stdout)
        self.assertNotIn("pull", result.stdout)

    def test_tsan_preflight_and_runtime_preserve_current_guards(self):
        before = run_body(self.named_step("Verify TSan image prerequisites"))
        for token in ("/usr/bin/llvm-symbolizer-14", "/opt/qt5-tsan/plugins/platforms/libqoffscreen.so",
                      "llvm-nm-14 -D --undefined-only", "libQt5Core.so.5", "__tsan_acquire", "__tsan_release"):
            self.assertIn(token, before)
        self.assertNotIn("grep -q", before)
        after = run_body(self.named_step("Verify TSan Qt runtime provenance"))
        for token in ("--env TSAN_OPTIONS", '"$KLOGG_WORKSPACE:/usr/local"',
                      "/usr/local/scripts/verify_tsan_qt_runtime.sh",
                      '"/usr/local/$KLOGG_BUILD_ROOT/output/klogg_itests"', "/opt/qt5-tsan"):
            self.assertIn(token, after)

    def test_instrumentation_checks_require_both_test_binaries_and_real_symbols(self):
        for name, symbol in (("Verify ASan-instrumented test binaries", "asan_init"),
                             ("Verify UBSan-instrumented test binaries", "ubsan_handle")):
            with self.subTest(name=name):
                body = run_body(self.named_step(name))
                self.assertIn("set -euo pipefail", body)
                self.assertIn("for bin in klogg_tests klogg_itests", body)
                self.assertIn('test -x "$f"', body)
                self.assertIn('nm "$f"', body)
                self.assertIn('grep "' + symbol + '" >/dev/null', body)
                commands = "\n".join(line for line in body.splitlines() if not line.lstrip().startswith("#"))
                self.assertNotIn("grep -q", commands)
                self.assertIn("exit 1", body)

    def test_instrumentation_payloads_fail_on_missing_symbols_binary_or_nm_error(self):
        for name, symbol in (("Verify ASan-instrumented test binaries", "__asan_init"),
                             ("Verify UBSan-instrumented test binaries", "__ubsan_handle_type_mismatch")):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory).resolve()
                output = root / "build_root/output"
                output.mkdir(parents=True)
                for binary in ("klogg_tests", "klogg_itests"):
                    (output / binary).write_text("fixture, never executed\n")
                    (output / binary).chmod(0o755)
                tools = root / "tools"
                tools.mkdir()
                nm = tools / "nm"
                nm.write_text('#!/bin/sh\nprintf "%s\\n" "$NM_FIXTURE_SYMBOL"\nexit "$NM_FIXTURE_STATUS"\n')
                nm.chmod(0o755)
                payload = shlex.split(run_body(self.named_step(name)))[-1].replace("/usr/local", str(root))
                env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                           KLOGG_BUILD_ROOT="build_root", NM_FIXTURE_SYMBOL=symbol, NM_FIXTURE_STATUS="0")
                def execute():
                    return subprocess.run(["bash", "-c", payload], env=env,
                                          check=False, capture_output=True, text=True)
                self.assertEqual(execute().returncode, 0)
                env["NM_FIXTURE_SYMBOL"] = "unrelated_symbol"
                self.assertNotEqual(execute().returncode, 0)
                env["NM_FIXTURE_SYMBOL"] = symbol
                env["NM_FIXTURE_STATUS"] = "1"
                self.assertNotEqual(execute().returncode, 0)
                env["NM_FIXTURE_STATUS"] = "0"
                (output / "klogg_itests").unlink()
                result = execute()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("test binary missing: klogg_itests", result.stdout)

    def test_tsan_preflight_payload_rejects_each_missing_prerequisite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            tools = root / "tools"
            tools.mkdir()
            symbolizer = root / "symbolizer"
            symbolizer.write_text("fixture, never executed\n")
            symbolizer.chmod(0o755)
            plugin = root / "qt/plugins/platforms/libqoffscreen.so"
            plugin.parent.mkdir(parents=True)
            plugin.write_text("fixture, never loaded\n")
            nm = tools / "llvm-nm-14"
            nm.write_text('#!/bin/sh\nprintf "%s\\n" "$TSAN_FIXTURE_SYMBOLS"\n')
            nm.chmod(0o755)
            payload = shlex.split(run_body(self.named_step("Verify TSan image prerequisites")))[-1]
            payload = payload.replace("/usr/bin/llvm-symbolizer-14", str(symbolizer)).replace("/opt/qt5-tsan", str(root / "qt"))
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       TSAN_FIXTURE_SYMBOLS="__tsan_acquire\n__tsan_release")
            def execute():
                return subprocess.run(["bash", "-c", payload], env=env,
                                      check=False, capture_output=True, text=True)
            self.assertEqual(execute().returncode, 0)
            for single_symbol in ("__tsan_acquire", "__tsan_release"):
                env["TSAN_FIXTURE_SYMBOLS"] = single_symbol
                self.assertNotEqual(execute().returncode, 0)
            env["TSAN_FIXTURE_SYMBOLS"] = "__tsan_acquire\n__tsan_release"
            plugin.unlink()
            self.assertNotEqual(execute().returncode, 0)
            plugin.write_text("restored fixture\n")
            symbolizer.unlink()
            self.assertNotEqual(execute().returncode, 0)

    def test_host_workspace_boundary_mutation_is_observably_wrong(self):
        body = run_body(action_steps()[0])
        broken = body.replace('environment["KLOGG_WORKSPACE"] = workspace',
                              'environment["KLOGG_WORKSPACE"] = "/usr/local"')
        self.assertNotEqual(broken, body)
        result, env, workspace = self.configuration(body=broken)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(env["KLOGG_WORKSPACE"], workspace)
        self.assertEqual(env["KLOGG_WORKSPACE"], "/usr/local")


if __name__ == "__main__":
    unittest.main()
