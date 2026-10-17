"""Exercise wrapper scripts without building images or running real containers."""
import importlib.util
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
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

    def test_shared_build_and_test_launches_enable_runtime_init_before_image(self):
        image = "zeacent/klogg_ubuntu22.04"
        for action, name in (("docker-build", "configure"), ("docker-build", "build"),
                             ("docker-run-tests", "test")):
            with self.subTest(action=action, step=name), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory).resolve()
                argv = root / "docker-argv"
                text = (ROOT / ".github/actions" / action / "action.yml").read_text()
                step = next(step for step in action_steps(text)
                            if QUALITY.workflow_step_fields(step)[0].get("name") == name)
                body = run_body(step).replace("${{ env.KLOGG_CI_IMAGE_PREFIX }}", "zeacent/klogg")
                body = body.replace("${{ inputs.container-suffix }}", "_ubuntu22.04")
                # Execute the real wrapper but record Docker argv without a daemon
                # or container payload. nproc is a deterministic host-side input.
                fixture = 'docker() { printf "%s\\0" "$@" > "$DOCKER_ARGV"; };\n'
                fixture += 'nproc() { printf "2\\n"; };\n'
                # MODEL host identity: never query this machine's UID/GID.
                fixture += 'id() { case "$1" in -u) printf "21123\\n";; -g) printf "21124\\n";; *) return 1;; esac; };\n'
                env = dict(os.environ, DOCKER_ARGV=str(argv), KLOGG_WORKSPACE=str(root / "workspace with spaces"),
                           KLOGG_VERSION="fixture-version", KLOGG_BUILD_ROOT="build_root",
                           KLOGG_CMAKE_OPTS="-DFIXTURE=--init", KLOGG_SANITIZER="",
                           GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="2")
                result = subprocess.run(["sh", "-eu", "-c", fixture + body], env=env,
                                        check=False, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                command = argv.read_bytes().decode().split("\0")[:-1]
                image_index = command.index(image)
                options = command[:image_index]
                self.assertEqual(options[:3], ["run", "--pull=never", "--init"],
                                 "launch must enable runtime init after the strict no-pull prefix, before the image")
                self.assertEqual([value for value in options if value == "--init" or value.startswith("--init=")],
                                 ["--init"])
                self.assertEqual(command[image_index + 1], "/bin/bash")
                self.assertEqual(command[command.index("-v") + 1], env["KLOGG_WORKSPACE"] + ":/usr/local")
                for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS"):
                    self.assertIn(option, options)
                if name == "configure":
                    self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", command[-1])
                    self.assertIn("-DCPM_SOURCE_CACHE=/usr/local/cpm_cache", command[-1])
                elif name == "build":
                    self.assertIn("ci-build-metrics-123-2.json", command[-1])
                    self.assertIn("-- cmake --build /usr/local/build_root --target ci_build", command[-1])
                else:
                    self.assertIn("LANG=C.UTF-8", options)
                    self.assertIn("ctest --output-on-failure --parallel 2", command[-1])

    def test_ordinary_analysis_jobs_enable_init_unconditionally_on_existing_events(self):
        for workflow, job in (("coverage.yml", "coverage"), ("static-analysis.yml", "static-analysis"),
                              ("codeql-analysis.yml", "analyze")):
            text = (ROOT / ".github/workflows" / workflow).read_text()
            triggers = QUALITY.workflow_trigger_mapping(text)
            self.assertIsNotNone(triggers)
            self.assertEqual(set(triggers), {"pull_request", "push", "workflow_dispatch"})
            block = QUALITY.workflow_job_blocks(text)[job]
            container = QUALITY.workflow_mapping_block(block, "container", 4)
            self.assertIsNotNone(container)
            for event in triggers:
                with self.subTest(workflow=workflow, event=event):
                    self.assertEqual(container["image"][0], "${{ needs.ResolveLinuxEnvironment.outputs.image }}")
                    self.assertEqual(container.get("options", (None, []))[0], "--init",
                                     "ordinary analysis needs a direct unconditional runtime init option")

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


class DockerBuildNamespaceActionTest(unittest.TestCase):
    """MODEL selector/argv tests, not native-container ownership qualification."""

    def body(self, name, text=None, suffix="_ubuntu22.04"):
        text = (ROOT / ".github/actions/docker-build/action.yml").read_text() if text is None else text
        step = next(step for step in action_steps(text) if QUALITY.workflow_step_fields(step)[0].get("name") == name)
        return (run_body(step).replace("${{ env.KLOGG_CI_IMAGE_PREFIX }}", "zeacent/klogg")
                .replace("${{ inputs.container-suffix }}", suffix))

    def execute(self, name, *, body=None, statuses="0", inherited=None):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            calls = root / "calls.jsonl"
            sleeps = root / "sleeps"
            model = root / "docker_model.py"
            model.write_text(textwrap.dedent('''\
                import json
                import os
                import pathlib
                import sys
                calls = pathlib.Path(os.environ["DOCKER_MODEL_CALLS"])
                previous = calls.read_text().splitlines() if calls.exists() else []
                with calls.open("a") as stream:
                    stream.write(json.dumps(sys.argv[1:]) + "\\n")
                if os.environ.get("DOCKER_MODEL_REQUIRE_NAMESPACE") == "1":
                    argv = sys.argv[1:]
                    options = argv[:argv.index("/bin/bash") - 1]
                    users = [options[index + 1] for index, value in enumerate(options) if value == "--user"]
                    homes = [options[index + 1] for index, value in enumerate(options)
                             if value == "--env" and options[index + 1].startswith("HOME=")]
                    # MODEL mount ancestry is host-owned; default image identity
                    # cannot materialize verified sources. No real owner is probed.
                    if users != [os.environ["MODEL_UID"] + ":" + os.environ["MODEL_GID"]] or homes != ["HOME=/tmp/klogg-build-home"]:
                        print("MODEL namespace identity/HOME rejected before configure/build", file=sys.stderr)
                        sys.exit(86)
                    print("MODEL payload entered")
                statuses = os.environ["DOCKER_MODEL_STATUSES"].split(",")
                sys.exit(int(statuses[min(len(previous), len(statuses) - 1)]))
                '''))
            fixture = '''\
                docker() { "$MODEL_PYTHON" "$DOCKER_MODEL" "$@"; }
                id() {
                  case "$1" in
                    -u) printf '%s\\n' "$MODEL_UID"; return "$MODEL_UID_STATUS" ;;
                    -g) printf '%s\\n' "$MODEL_GID"; return "$MODEL_GID_STATUS" ;;
                    *) return 1 ;;
                  esac
                }
                sleep() { printf '%s\\n' "$@" >> "$MODEL_SLEEPS"; }
                '''
            env = dict(os.environ, MODEL_PYTHON=sys.executable, DOCKER_MODEL=str(model),
                       DOCKER_MODEL_CALLS=str(calls), DOCKER_MODEL_STATUSES=statuses,
                       MODEL_SLEEPS=str(sleeps), MODEL_UID="21123", MODEL_GID="21124",
                       MODEL_UID_STATUS="0", MODEL_GID_STATUS="0", HOME="/root",
                       KLOGG_WORKSPACE=str(root / "workspace with spaces"),
                       KLOGG_VERSION="model-version", KLOGG_BUILD_ROOT="build_root",
                       KLOGG_CMAKE_OPTS="-DFIXTURE=ON", KLOGG_SANITIZER="",
                       ASAN_OPTIONS="model-asan", UBSAN_OPTIONS="model-ubsan",
                       LSAN_OPTIONS="model-lsan", TSAN_OPTIONS="model-tsan",
                       GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="2")
            env.update(inherited or {})
            result = subprocess.run(["sh", "-eu", "-c", fixture + (body if body is not None else self.body(name))],
                                    cwd=ROOT / "tests/scripts", env=env, check=False, capture_output=True, text=True)
            commands = [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []
            backoffs = sleeps.read_text().splitlines() if sleeps.exists() else []
            return result, commands, backoffs

    def test_configure_and_build_project_host_identity_and_owned_home_before_payload(self):
        for name in ("configure", "build"):
            with self.subTest(step=name):
                result, calls, sleeps = self.execute(name, inherited={"DOCKER_MODEL_REQUIRE_NAMESPACE": "1",
                                                                     "KLOGG_CONTAINER_USER": "0:0"})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((len(calls), sleeps), (1, []))
                self.assertIn("MODEL payload entered", result.stdout)

    def test_real_action_mutations_cannot_drop_spoof_or_override_namespace_projection(self):
        text = (ROOT / ".github/actions/docker-build/action.yml").read_text()
        for old, new in (
                ('--user "$host_uid:$host_gid"', '--user "0:0"'),
                ('--user "$host_uid:$host_gid"', '# --user "$host_uid:$host_gid"'),
                ('--env HOME=/tmp/klogg-build-home', '--env HOME=/root'),
                ('--env HOME=/tmp/klogg-build-home', '--env HOME="$HOME"')):
            self.assertIn(old, text)
            mutated = text.replace(old, new)
            for name in ("configure", "build"):
                with self.subTest(mutation=new, step=name):
                    # A shell comment in a continued command can terminate it;
                    # either shell failure or the MODEL selector gate must stop it.
                    result, _, _ = self.execute(name, body=self.body(name, mutated),
                                                inherited={"DOCKER_MODEL_REQUIRE_NAMESPACE": "1"})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn("MODEL payload entered", result.stdout)

    def test_projection_uses_current_host_identity_not_an_inherited_selector_or_uid_allowlist(self):
        for uid, gid in (("0", "0"), ("123", "456")):
            for name in ("configure", "build"):
                with self.subTest(uid=uid, gid=gid, step=name):
                    result, calls, sleeps = self.execute(name, inherited={"DOCKER_MODEL_REQUIRE_NAMESPACE": "1",
                                                                         "MODEL_UID": uid, "MODEL_GID": gid,
                                                                         "host_uid": "999", "host_gid": "999"})
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((len(calls), sleeps), (1, []))

    def test_pr_push_and_dispatch_call_paths_execute_the_shared_projection_for_each_build_role(self):
        from test_ci_environment_workflow import event_expression

        ordinary_text = (ROOT / ".github/workflows/ci-build.yml").read_text()
        ordinary = QUALITY.workflow_job_blocks(ordinary_text)
        ordinary_steps = QUALITY.workflow_job_steps(ordinary_text)
        producer_text = (ROOT / ".github/workflows/ci-environments.yml").read_text()
        producer = QUALITY.workflow_job_blocks(producer_text)
        producer_steps = QUALITY.workflow_job_steps(producer_text)
        for event, mode in (("pull_request", "off"), ("push", "off"),
                            ("workflow_dispatch", "off"), ("workflow_dispatch", "qualify")):
            roles = []
            for job in ("LinuxPackages", "LinuxSanitizers", "LinuxTsan"):
                condition = QUALITY.workflow_job_direct_value(ordinary[job], "if")
                if not event_expression(condition, event, mode):
                    continue
                steps = ordinary_steps[job]
                self.assertEqual(sum(QUALITY.workflow_step_fields(step)[0].get("uses") ==
                                     "./.github/actions/docker-build" for step in steps), 1)
                if job == "LinuxTsan":
                    env = QUALITY.workflow_mapping_block(ordinary[job], "env", 4)
                    roles.append((env["KLOGG_CONTAINER_SUFFIX"][0], env["KLOGG_SANITIZER"][0]))
                else:
                    matrix = QUALITY.workflow_job_matrix_values(ordinary[job])
                    roles.extend((suffix, sanitizer) for suffix in sorted(matrix["container_suffix"])
                                 for sanitizer in sorted(matrix["sanitizer"]))
            caller = ordinary["EnvironmentProducer"]
            if event_expression(QUALITY.workflow_job_direct_value(caller, "if"), event, mode):
                self.assertEqual(QUALITY.workflow_job_direct_value(caller, "uses"),
                                 "./.github/workflows/ci-environments.yml")
                for job, steps in producer_steps.items():
                    if not any(QUALITY.workflow_step_fields(step)[0].get("uses") ==
                               "./.github/actions/linux-validate" for step in steps):
                        continue
                    env = QUALITY.workflow_mapping_block(producer[job], "env", 4)
                    configured, values, _ = LinuxValidateActionTest().configuration(
                        env["KLOGG_FAMILY"][0], env["KLOGG_PROFILE"][0])
                    self.assertEqual(configured.returncode, 0, configured.stderr)
                    roles.append((values["KLOGG_CONTAINER_SUFFIX"], values["KLOGG_SANITIZER"]))
            # Six owner-failed roles plus the separate TSan caller. This is only
            # an event/profile/shell MODEL; it issues no qualification evidence.
            self.assertEqual(sorted(roles), sorted([("_ubuntu20.04", ""), ("_ubuntu22.04", ""),
                                                    ("_ubuntu24.04", ""), ("_ubuntu26.04", ""),
                                                    ("_ubuntu22.04", "address"), ("_ubuntu22.04", "undefined"),
                                                    ("_ubuntu22.04-tsan", "thread")]))
            for suffix, sanitizer in roles:
                for name in ("configure", "build"):
                    with self.subTest(event=event, mode=mode, suffix=suffix, sanitizer=sanitizer, step=name):
                        result, calls, sleeps = self.execute(name, body=self.body(name, suffix=suffix),
                                                            inherited={"DOCKER_MODEL_REQUIRE_NAMESPACE": "1",
                                                                       "KLOGG_SANITIZER": sanitizer,
                                                                       "GITHUB_EVENT_NAME": event})
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual((len(calls), sleeps), (1, []))
                        self.assertIn("MODEL payload entered", result.stdout)

    def test_payload_creates_writable_container_home_without_touching_source_or_object_cache(self):
        for name in ("configure", "build"):
            with self.subTest(step=name), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory).resolve()
                workspace = root / "workspace"
                workspace.mkdir()
                for cache in ("cpm_cache", ".ccache"):
                    (workspace / cache).mkdir()
                    (workspace / cache / "sentinel").write_text("retained MODEL cache\n")
                home = root / "model-image-tmp/home"
                result, calls, _ = self.execute(name)
                self.assertEqual(result.returncode, 0, result.stderr)
                payload = calls[0][-1].replace("/usr/local", str(workspace))
                fixture = '''\
                    model_payload() {
                      test -d "$HOME" || { printf 'MODEL HOME was not created\\n' >&2; return 1; }
                      printf '%s\\n' "$*" > "$HOME/payload"
                    }
                    cmake() { model_payload "$@"; }
                    python3() { model_payload "$@"; }
                    '''
                # Only shell/temp-file semantics are real. Image/toolchain,
                # identity, CMake and metrics are MODEL stubs, never executed.
                result = subprocess.run(["sh", "-eu", "-c", fixture + payload],
                                        cwd=root, env=dict(os.environ, HOME=str(home)),
                                        check=False, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((home / "payload").is_file())
                for cache in ("cpm_cache", ".ccache"):
                    self.assertEqual((workspace / cache / "sentinel").read_text(), "retained MODEL cache\n")

    def test_missing_malformed_or_failed_host_identity_never_enters_docker_or_retries(self):
        for name in ("configure", "build"):
            for key in ("MODEL_UID", "MODEL_GID"):
                for value in ("", "root", "-1", "12:34", "12 34", "12\n34"):
                    with self.subTest(step=name, key=key, value=value):
                        result, calls, sleeps = self.execute(name, inherited={key: value})
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual((calls, sleeps), ([], []))
            for key in ("MODEL_UID_STATUS", "MODEL_GID_STATUS"):
                with self.subTest(step=name, failed=key):
                    result, calls, sleeps = self.execute(name, inherited={key: "9"})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual((calls, sleeps), ([], []))

    def test_characterization_mount_toolchain_metrics_and_cache_modes_are_preserved(self):
        for name in ("configure", "build"):
            for sanitizer, cache in (("", "measured"), ("thread", "disabled")):
                with self.subTest(step=name, sanitizer=sanitizer):
                    result, calls, backoffs = self.execute(name, inherited={"KLOGG_SANITIZER": sanitizer})
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(len(calls), 1)
                    self.assertEqual(backoffs, [])
                    command = calls[0]
                    self.assertEqual(command[:3], ["run", "--pull=never", "--init"])
                    mount = command[command.index("-v") + 1]
                    self.assertTrue(mount.endswith("workspace with spaces:/usr/local"), mount)
                    self.assertEqual(command[-4:-1], ["zeacent/klogg_ubuntu22.04", "/bin/bash",
                                                    "-lc" if name == "configure" else "-c"])
                    for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS"):
                        self.assertIn(option, command[:-4])
                    if name == "configure":
                        self.assertIn("KLOGG_VERSION=model-version", command)
                        self.assertIn("-DFIXTURE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON", command[-1])
                        self.assertIn("-DCPM_SOURCE_CACHE=/usr/local/cpm_cache /usr/local", command[-1])
                    else:
                        self.assertIn("KLOGG_OBJECT_CACHE=" + cache, command)
                        self.assertEqual(command[-1].count("cmake --build"), 1)
                        self.assertIn("ci-build-metrics-123-2.json", command[-1])
                        self.assertIn("--object-cache " + cache, command[-1])
                        self.assertIn("-- cmake --build /usr/local/build_root --target ci_build", command[-1])

    def test_characterization_configure_retries_are_bounded_but_build_is_not_retried(self):
        for statuses, expected_status, attempts, backoffs in (
                ("7,0", 0, 2, ["20"]), ("7", 1, 3, ["20", "30"])):
            result, calls, sleeps = self.execute("configure", statuses=statuses)
            self.assertEqual(result.returncode, expected_status)
            self.assertEqual(len(calls), attempts)
            self.assertEqual(sleeps, backoffs)
            self.assertTrue(all(command == calls[0] for command in calls))
        result, calls, sleeps = self.execute("build", statuses="7")
        self.assertEqual((result.returncode, len(calls), sleeps), (7, 1, []))

    def test_characterization_conflicting_compile_database_option_never_enters_docker(self):
        for value in ("OFF", "0"):
            result, calls, sleeps = self.execute("configure", inherited={"KLOGG_CMAKE_OPTS":
                                                                        "-DCMAKE_EXPORT_COMPILE_COMMANDS=" + value})
            self.assertEqual((result.returncode, calls, sleeps), (1, [], []))
            self.assertIn("conflicts with the required compile database", result.stdout)


if __name__ == "__main__":
    unittest.main()
