"""Analysis recipe and executable workflow namespace contracts.

The workflow model assigns root to the job and a foreign UID to the mounted
workspace. It never changes real ownership or runs containers/native analysis.
"""
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RECIPE = ROOT / "docker/ubuntu24.04-analysis/Dockerfile"


class AnalysisEnvironmentRecipeTest(unittest.TestCase):
    def recipe(self):
        self.assertTrue(RECIPE.is_file(), "the shared analysis environment recipe is missing")
        return RECIPE.read_text(encoding="utf-8")

    def test_packages_are_installed_from_verified_offline_materials(self):
        recipe = self.recipe()
        self.assertIn("FROM ${UBUNTU_IMAGE}", recipe)
        self.assertIn("ARG APT_RUNTIME_LOCK_SHA256", recipe)
        self.assertIn("ci_install_locked_apt.sh", recipe)
        self.assertIn("RUN --network=none", recipe)
        self.assertNotIn("apt-get update", recipe)
        self.assertNotIn("apt-get install", recipe)
        self.assertNotIn("curl ", recipe)
        self.assertNotIn("wget ", recipe)

    def test_tools_are_hash_checked_and_safely_extracted_before_execution(self):
        recipe = self.recipe()
        check = recipe.index("sha256sum --check --strict")
        extract = recipe.index("python3 /opt/klogg-ci/extract_verified_tar.py")
        probe = recipe.index("cmake --version")
        self.assertLess(check, extract)
        self.assertLess(extract, probe)
        self.assertIn("ARG TOOLS_ARCHIVE_SHA256", recipe)
        self.assertIn("prefetch_adb_helper_sources.py", recipe)

    def test_shared_analysis_tools_match_existing_roles_without_codeql_redistribution(self):
        recipe = self.recipe()
        for version in ("3.31.6", "1.12.1", "6.9.3"):
            self.assertIn(version, recipe)
        self.assertIn("CC=gcc-13", recipe)
        self.assertIn("CXX=g++-13", recipe)
        self.assertIn("CMAKE_PREFIX_PATH=/opt/klogg-tools/qt/6.9.3/gcc_64", recipe)
        self.assertIn("LANG=C.UTF-8", recipe)
        active = "\n".join(line for line in recipe.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("codeql", active.lower())
        self.assertNotIn("/usr/local/cpm_cache", active)
        self.assertNotIn("GITHUB_SHA", active)


def model_tool(tool, arguments):
    """Run a tool seam against modeled ownership, with real fixture Git only."""
    state_path = pathlib.Path(os.environ["MODEL_STATE"])
    state = json.loads(state_path.read_text())
    workspace = pathlib.Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = pathlib.Path(os.environ["MODEL_TMP"])
    cwd = pathlib.Path.cwd().resolve()
    state["calls"].append({"tool": tool, "args": arguments, "cwd": str(cwd)})

    def require(condition, message):
        if not condition:
            raise ValueError("MODEL " + message)

    def path(value):
        return (cwd / value).resolve()

    def option(name):
        return arguments[arguments.index(name) + 1]

    def build_path(value):
        build = path(value)
        private = pathlib.Path(state.get("private", workspace))
        require(state.get("job_uid") == 0 and state.get("workspace_uid") != 0,
                "fixture must model root over a foreign-owned workspace")
        require("private" in state and private in build.parents,
                "private namespace ancestor has an untrusted owner")
        require(private.stat().st_ino == state["private_inode"],
                "acquired namespace parent was replaced")
        return build

    # The wrapper and shared executor are real. Only external analysis tools
    # are seams; shell functions would disappear across the Python boundary.
    if tool == "python3" and pathlib.Path(arguments[0]).name == "qualify_ci_analysis.py":
        require(path(arguments[0]) == workspace / "scripts/qualify_ci_analysis.py",
                "ordinary analysis helper must execute from the original checkout")
        phase = arguments[2]
        expected_source = (pathlib.Path(state["private"]) / "source"
                           if state["role"] == "coverage" and phase != "coverage-ratchet" else workspace)
        require(path(option("--workspace")) == expected_source, "shared phase changed its workspace")
        require(option("--role") == state["role"], "shared phase changed its role")
        if phase == "coverage-ratchet":
            require("--build-dir" not in arguments, "ratchet must not depend on the private build")
        state_path.write_text(json.dumps(state))
        os.execv(os.environ["MODEL_PYTHON"], [os.environ["MODEL_PYTHON"]] + arguments)

    try:
        if os.environ.get("MODEL_FAIL_TOOL") == tool:
            raise ValueError("MODEL injected " + tool + " failure")
        if tool == "mktemp":
            if "-d" in arguments:
                require(not os.environ.get("MODEL_FAIL_ALLOCATION"), "allocation failed")
                require(len(arguments) == 2 and arguments[1].startswith(str(temporary / "klogg-analysis.")),
                        "private allocation must explicitly use container /tmp")
                private = pathlib.Path(tempfile.mkdtemp(prefix="klogg-analysis.", dir=str(temporary)))
                state.update(private=str(private), private_inode=private.stat().st_ino)
                print(private)
            else:
                descriptor, name = tempfile.mkstemp(dir=str(temporary))
                os.close(descriptor)
                print(name)
        elif tool == "git":
            if "clone" in arguments:
                require("--no-hardlinks" in arguments and "--no-checkout" in arguments,
                        "clone must be an independent no-checkout local copy")
                require(not any(value in arguments for value in ("--shared", "--reference", "--reference-if-able")),
                        "clone must not share object storage")
                require(path(arguments[-2]) == workspace, "clone must read the actual checkout, not a remote")
                require(build_path(arguments[-1]).parent == pathlib.Path(state["private"]),
                        "clone must live beneath the retained private parent")
                trust = subprocess.run([os.environ["MODEL_GIT"], "config", "--global", "--get-all", "safe.directory"],
                                       capture_output=True, text=True, check=False)
                require(str(workspace / ".git") in trust.stdout.splitlines(),
                        "foreign source clone needs exact git-directory trust")
            if "config" in arguments and "safe.directory" in arguments:
                require(arguments[-1] in (str(workspace), str(workspace / ".git")),
                        "Git trust must name only the original exact source")
            require("fetch" not in arguments, "fixture must not use network fetch")
            result = subprocess.run([os.environ["MODEL_GIT"]] + arguments, check=False)
            require(result.returncode == 0, "fixture Git failed")
        elif tool == "rm":
            for value in arguments:
                if value.startswith("-"):
                    continue
                target = path(value)
                fixture = pathlib.Path(os.environ["MODEL_FIXTURE"])
                require(fixture in target.parents, "cleanup must stay inside the fixture")
                if "private" in state:
                    private = pathlib.Path(state["private"])
                    require(target != private and target not in private.parents,
                            "cleanup must retain the authority-bearing namespace parent")
                    require(private in target.parents, "build cleanup must stay inside the acquired namespace")
            result = subprocess.run([os.environ["MODEL_RM"]] + arguments, check=False)
            require(result.returncode == 0, "fixture cleanup failed")
        elif tool == "cmake":
            if "--build" in arguments:
                build = build_path(option("--build"))
                require(str(build) == state.get("build"), "build consumer changed its configured path")
                if state["role"] == "codeql":
                    if "klogg_codeql_thirdparty" in arguments:
                        require(not state.get("codeql_initialized"), "vendored prebuild must precede CodeQL init")
                        state["prebuilt"] = True
                    elif "-n" in arguments:
                        require(state.get("prebuilt") and not state.get("codeql_initialized"),
                                "traced plan must follow prebuild and precede init")
                        state["plan_checked"] = True
                        print("MODEL first-party compile src/example.cpp")
                    else:
                        require(state.get("codeql_initialized"), "application build must be traced")
                        state["traced"] = True
            else:
                source = path(option("-S") if "-S" in arguments else arguments[-1])
                build = build_path(option("-B") if "-B" in arguments else str(cwd))
                expected_source = pathlib.Path(state["private"]) / "source" if state["role"] == "coverage" else workspace
                require(source == expected_source, "configure source changed or coverage source is not private")
                require(state["role"] != "coverage" or source in build.parents,
                        "coverage build must remain inside its gcovr source root")
                cache = next(value.split("=", 1)[1] for value in arguments if value.startswith("-DCPM_SOURCE_CACHE="))
                require(path(cache) == workspace / "cpm_cache", "configure must retain the original authenticated cache")
                require("-DFETCHCONTENT_FULLY_DISCONNECTED=ON" in arguments, "configure must remain disconnected")
                build.mkdir(parents=True, exist_ok=True)
                database = [{"directory": str(build), "file": str(filename),
                             "arguments": ["c++", "-c", str(filename)]}
                            for filename in (source / "src/example.cpp", workspace / "cpm_cache/efsw/src/vendor.cpp")]
                (build / "compile_commands.json").write_text(json.dumps(database))
                state.update(build=str(build), source=str(source))
        elif tool == "ctest":
            require(str(build_path(str(cwd))) == state.get("build"), "CTest changed build directory")
            require("--output-on-failure" in arguments and "--parallel" in arguments, "CTest policy changed")
            state["tested"] = True
        elif tool == "gcovr":
            source = path(option("--root"))
            build = build_path(arguments[-1])
            require(str(source) == state.get("source") and source in build.parents,
                    "gcovr source/build geometry changed")
            require(str(build) == state.get("build") and state.get("tested"), "gcovr must consume the tested build")
            require(option("--filter") == "^src/", "coverage filter changed")
            output = path(option("-o"))
            require(cwd in (workspace, source) and output.parent == cwd / "coverage_report",
                    "coverage reports must use the reporting working directory")
            if output.name == "index.html":
                require({"--html-details", "--html-self-contained", "--html-title", "--print-summary"}.issubset(arguments)
                        and "--json" not in arguments, "HTML report mode/summary arguments changed")
                require(option("--html-title") == "Klogg line/branch coverage", "HTML report title changed")
            elif output.name == "coverage.json":
                require({"--json", "--json-pretty"}.issubset(arguments)
                        and not {"--html-details", "--print-summary"}.intersection(arguments), "JSON report mode changed")
            else:
                raise ValueError("MODEL unexpected coverage report name")
            state.setdefault("coverage_reports", []).append(output.name)
            # Explicit relative gcovr filters are CWD-relative, not --root-relative.
            # These candidate counters and report bytes remain synthetic.
            matches = lambda filename: bool(re.match(option("--filter"), os.path.relpath(str(filename), str(cwd))))
            selected = matches(source / "src/example.cpp")
            state.setdefault("coverage_selections", []).append({
                "cwd": str(cwd), "first_party": selected,
                "original_checkout": matches(workspace / "src/example.cpp"),
                "vendor": matches(workspace / "cpm_cache/efsw/src/vendor.cpp"),
            })
            output.write_text(json.dumps({"files": [{"file": "src/example.cpp"}] if selected else []}))
            if output.name == "index.html":
                (output.parent / "example.cpp.html").write_text("synthetic detail report\n")
            if "--print-summary" in arguments:
                measured = "100.0% (1 out of 1)" if selected else "0.0% (0 out of 0)"
                print("lines: " + measured + "\nbranches: " + measured)
        elif tool == "python3":
            script = pathlib.Path(arguments[0]).name
            if script == "enforce_coverage_ratchet.py":
                require(cwd == workspace and path(arguments[0]) == workspace / "scripts" / script,
                        "ratchet must use original checkout context")
                require(option("--base-sha") == os.environ["PR_BASE_SHA"], "ratchet base changed")
                require(path(option("--summary")) == workspace / "coverage_report/summary.txt",
                        "ratchet summary moved")
                for name, filename in (("--line-floor", "coverage-line.txt"), ("--branch-floor", "coverage-branch.txt")):
                    require(path(option(name)) == workspace / ".github/baselines" / filename, "ratchet floor moved")
                require(sorted(state.get("coverage_reports", [])) == ["coverage.json", "index.html"],
                        "ratchet requires both reports from this execution")
                state_path.write_text(json.dumps(state))
                result = subprocess.run([os.environ["MODEL_PYTHON"]] + arguments,
                                        check=False, capture_output=True, text=True)
                state = json.loads(state_path.read_text())
                require(result.returncode == 0, "coverage summary rejected: " + result.stdout + result.stderr)
                state["ratcheted"] = True
            else:
                require(cwd == workspace, "static tools must retain original source context")
                build = pathlib.Path(state["build"])
                if script == "run_changed_clang_tidy.py":
                    require(path(option("--build-dir")) == build, "clang-tidy used another build")
                    require(option("--base") == os.environ["KLOGG_ANALYSIS_BASE"], "clang-tidy base changed")
                    require(option("--clang-tidy-diff") == os.environ["CLANG_TIDY_DIFF"], "clang-tidy-diff changed")
                else:
                    raise ValueError("MODEL unexpected Python script: " + script)
        elif tool == "clang-tidy":
            require(path(option("-p")) == pathlib.Path(state["build"]) and cwd == workspace,
                    "clang-tidy source/build changed")
        elif tool == "cppcheck":
            project = next(value.split("=", 1)[1] for value in arguments if value.startswith("--project="))
            require(path(project).is_file() and cwd == workspace, "cppcheck source/database changed")
            require(path(project).parent == pathlib.Path(state["build"]), "cppcheck scratch escaped private build")
            records = json.loads(path(project).read_text())
            require([record["file"] for record in records] == [str(workspace / "src/example.cpp")],
                    "cppcheck must analyze first-party units and exclude the cache")
            require(("--error-exitcode=1" in arguments) == (os.environ["KLOGG_ANALYSIS_MODE"] == "changed-strict"),
                    "cppcheck strictness changed")
        elif tool == "codeql-init":
            require(state.get("prebuilt") and state.get("plan_checked"), "CodeQL init preceded prebuild/plan")
            state["codeql_initialized"] = True
        elif tool == "codeql-analyze":
            require(state.get("traced"), "CodeQL analyzed without the traced application build")
        elif tool == "upload":
            require(arguments == ["coverage_report/"] and state.get("ratcheted"), "upload/ratchet context changed")
            require(all((workspace / "coverage_report" / name).is_file()
                        for name in ("index.html", "summary.txt", "coverage.json")), "coverage uploads are missing")
            files = json.loads((workspace / "coverage_report/coverage.json").read_text())["files"]
            require(files == [{"file": "src/example.cpp"}], "JSON report omitted the measured private source")
        else:
            raise ValueError("MODEL unexpected tool: " + tool)
    finally:
        state_path.write_text(json.dumps(state))


@unittest.skipUnless(os.name == "posix", "Linux job scripts require a POSIX shell and filesystem")
class AnalysisWorkflowNamespaceTest(unittest.TestCase):
    """MODEL ownership/path contracts, not native-container or gcovr acceptance."""

    ROLES = {
        "static": ("static-analysis.yml", "static-analysis"),
        "codeql": ("codeql-analysis.yml", "analyze"),
        "coverage": ("coverage.yml", "coverage"),
    }
    CONSUMERS = {
        "static": {"Configure (generate compile_commands.json)", "Generate Qt UI headers for analysis",
                   "Run clang-tidy", "Run cppcheck (warning+style over src/)"},
        "codeql": {"Configure traced build", "Prebuild vendored dependencies outside CodeQL tracing",
                   "Verify traced build contains only first-party work", "Build traced application"},
        "coverage": {"Configure (Debug + coverage)", "Build tests", "Run tests", "Generate coverage report",
                     "Enforce coverage ratchet"},
    }

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("analysis_namespace_quality", ROOT / "scripts/lint_ci_quality.py")
        cls.quality = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.quality)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="analysis-namespace-", dir=str(ROOT / "tests/scripts"))
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        # Profile environment rendering deliberately accepts only canonical
        # shell-safe workspace paths, as used by the Linux job mount.
        self.workspace = self.root / "foreign-workspace"
        self.workspace.mkdir()
        self.tmp = self.root / "container-tmp"
        self.tmp.mkdir()
        self.state = self.root / "state.json"
        self.github_env = self.root / "github-env"
        self.environment = dict(os.environ)
        for name in list(self.environment):
            if name.startswith("GIT_"):
                del self.environment[name]
        self.environment.update(
            HOME=str(self.root), GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=str(self.root / "job-gitconfig"),
            GIT_AUTHOR_NAME="Namespace fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
            GIT_COMMITTER_NAME="Namespace fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid",
            MODEL_FIXTURE=str(self.root), MODEL_TMP=str(self.tmp), MODEL_STATE=str(self.state),
            MODEL_PYTHON=sys.executable, MODEL_MODULE=str(pathlib.Path(__file__).resolve()),
            PYTHONDONTWRITEBYTECODE="1",
            MODEL_GIT=shutil.which("git"), MODEL_RM=shutil.which("rm"),
            GITHUB_WORKSPACE=str(self.workspace), GITHUB_ENV=str(self.github_env),
            KLOGG_WORKSPACE=str(self.workspace), KLOGG_BUILD_ROOT="build_root",
            KLOGG_ANALYSIS_MODE="changed-strict", CLANG_TIDY_DIFF="fixture-tidy-diff", EVENT_NAME="pull_request",
        )
        self.git("init", "-q", "-b", "main")
        for name, content in (("src/example.cpp", "int base;\n"),
                              (".github/baselines/coverage-line.txt", "1\n"), (".github/baselines/coverage-branch.txt", "1\n")):
            path = self.workspace / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        for relative in ("scripts/run_linux_analysis.sh", "scripts/qualify_ci_analysis.py",
                         "scripts/ci_build_metrics.py", "scripts/ci_environment.py", "scripts/ci_environment_profiles.py",
                         "scripts/first_party_compile_units.py", "scripts/filter_compile_database.py",
                         "scripts/prefetch_adb_helper_sources.py", "scripts/enforce_coverage_ratchet.py",
                         "ci/environments/profiles.json"):
            destination = self.workspace / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.helper = self.workspace / "scripts/qualify_ci_analysis.py"
        self.helper_source = self.helper.read_text()
        tools = self.root / "tools"
        tools.mkdir()
        for tool in ("mktemp", "git", "rm", "cmake", "ctest", "gcovr", "python3", "cppcheck", "clang-tidy"):
            executable = tools / tool
            executable.write_text('#!/bin/sh\nexec "$MODEL_PYTHON" "$MODEL_MODULE" --analysis-model '
                                  + tool + ' "$@"\n')
            executable.chmod(0o755)
        for tool in ("nproc", "getconf"):
            executable = tools / tool
            executable.write_text('#!/bin/sh\nprintf "2\\n"\n')
            executable.chmod(0o755)
        self.environment["PATH"] = str(tools) + os.pathsep + os.environ.get("PATH", "")
        self.git("add", ".")
        self.git("commit", "-qm", "base")
        self.base = self.git("rev-parse", "HEAD")
        self.git("checkout", "-qb", "feature")
        (self.workspace / "src/example.cpp").write_text("int feature;\n")
        self.git("commit", "-qam", "feature")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "feature", "-m", "merge")
        self.head = self.git("rev-parse", "HEAD")
        self.git("reset", "--hard", self.base)
        self.git("checkout", "-q", "--detach", self.head)
        cache = self.workspace / "cpm_cache/efsw"
        cache.mkdir(parents=True)
        (cache / "approved.txt").write_text("approved cache bytes\n")
        self.environment.update(PR_BASE_SHA=self.base, KLOGG_ANALYSIS_BASE=self.base)
        self.original = self.source_inventory()

    def git(self, *arguments):
        return subprocess.run([self.environment["MODEL_GIT"]] + list(arguments), cwd=self.workspace,
                              env=self.environment, check=True, capture_output=True, text=True).stdout.strip()

    def source_inventory(self):
        return {str(path.relative_to(self.workspace)): (path.read_bytes(), path.stat().st_mode, path.stat().st_ino,
                                                       path.stat().st_nlink)
                for path in self.workspace.rglob("*") if path.is_file()
                and "coverage_report" not in path.relative_to(self.workspace).parts}

    def workflow(self, role):
        return (ROOT / ".github/workflows" / self.ROLES[role][0]).read_text()

    def steps(self, role, text):
        return self.quality.workflow_job_steps(text)[self.ROLES[role][1]]

    def execute(self, role, text=None, fail_allocation=False, static_mode="full-report",
                helper_mutation=None, event="pull_request", fail_tool=None):
        helper = self.helper_source
        if helper_mutation:
            old, new = helper_mutation
            self.assertIn(old, helper)
            helper = helper.replace(old, new, 1)
        self.helper.write_text(helper)
        self.state.write_text(json.dumps({"calls": [], "role": role, "job_uid": 0, "workspace_uid": 21123}))
        self.github_env.write_text("")
        # Each scenario models a fresh job: prior upload data cannot mask an
        # omitted report transport from the new private checkout.
        shutil.rmtree(self.workspace / "coverage_report", ignore_errors=True)
        global_config = pathlib.Path(self.environment["GIT_CONFIG_GLOBAL"])
        if global_config.exists():
            global_config.unlink()
        environment = dict(self.environment, KLOGG_ANALYSIS_MODE=static_mode, EVENT_NAME=event,
                           PUSH_BASE_SHA=self.base)
        if fail_allocation:
            environment["MODEL_FAIL_ALLOCATION"] = "1"
        if fail_tool:
            environment["MODEL_FAIL_TOOL"] = fail_tool
        result = None
        for step in self.steps(role, text if text is not None else self.workflow(role)):
            fields, children = self.quality.workflow_step_fields(step)
            name, action = fields.get("name", ""), fields.get("uses", "")
            seam = None
            if action.startswith("github/codeql-action/"):
                seam = "codeql-init" if "/init@" in action else "codeql-analyze"
            elif role == "coverage" and action.startswith("actions/upload-artifact@"):
                seam = "upload"
            elif not (name.startswith("Acquire private ") or name == "Trust the workspace for Git inside the container"
                      or name in self.CONSUMERS[role]):
                continue
            if seam:
                arguments = [children.get("with", {}).get("path", "")] if seam == "upload" else []
                command = [sys.executable, str(pathlib.Path(__file__).resolve()), "--analysis-model", seam] + arguments
                cwd = self.workspace
            else:
                script = fields["run"].replace("/tmp/", str(self.tmp) + "/")
                working = fields.get("working-directory", str(self.workspace))
                working = re.sub(r"\$\{\{ env\.([A-Z_]+) \}\}", lambda match: environment[match.group(1)], working)
                cwd = pathlib.Path(working)
                command = ["bash", "--noprofile", "--norc", "-euo", "pipefail", "-c", script]
            result = subprocess.run(command, cwd=cwd, env=environment, check=False, capture_output=True, text=True)
            if result.returncode:
                break
            for line in self.github_env.read_text().splitlines():
                key, value = line.split("=", 1)
                environment[key] = value
        self.assertEqual(self.helper.read_text(), helper, "analysis changed the original executor")
        self.helper.write_text(self.helper_source)
        self.assertEqual(self.source_inventory(), self.original, "original checkout/cache changed")
        return result, json.loads(self.state.read_text())

    def test_actual_analysis_workflows_acquire_private_builds_and_preserve_consumers(self):
        for role in self.ROLES:
            with self.subTest(role=role):
                result, state = self.execute(role)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                private = pathlib.Path(state["private"])
                self.assertEqual(private.stat().st_ino, state["private_inode"])
                self.assertEqual(private.stat().st_mode & 0o777, 0o700)
                self.assertIn(private, pathlib.Path(state["build"]).parents)
                if role in ("static", "coverage"):
                    shared = [call for call in state["calls"] if call["tool"] == "python3"
                              and pathlib.Path(call["args"][0]).name == "qualify_ci_analysis.py"]
                    expected = (["configure", "static-prepare", "static-tidy", "static-cppcheck"]
                                if role == "static" else ["configure", "coverage-build", "coverage-test",
                                                          "coverage-report", "coverage-ratchet"])
                    self.assertEqual([call["args"][2] for call in shared], expected)
                    for step in self.steps(role, self.workflow(role)):
                        fields, _ = self.quality.workflow_step_fields(step)
                        if fields.get("name", "").startswith("Configure ("):
                            self.assertNotRegex(fields["run"], r"\b(?:rm|mkdir)\b")
                if role == "coverage":
                    source = pathlib.Path(state["source"])
                    actual = subprocess.run([self.environment["MODEL_GIT"], "-C", str(source), "rev-parse", "HEAD"],
                                            env=self.environment, check=True, capture_output=True, text=True).stdout.strip()
                    self.assertEqual(actual, self.head)
                    self.assertEqual((source / "src/example.cpp").read_bytes(), (self.workspace / "src/example.cpp").read_bytes())
                    self.assertNotEqual(actual, self.git("rev-parse", "main"))
                    self.assertFalse((source / ".git/objects/info/alternates").exists())
                    for original in (self.workspace / ".git/objects").rglob("*"):
                        if original.is_file():
                            copied = source / original.relative_to(self.workspace)
                            self.assertNotEqual((original.stat().st_dev, original.stat().st_ino),
                                                (copied.stat().st_dev, copied.stat().st_ino))

    def test_allocation_failure_stops_before_configure_or_clone(self):
        for role in self.ROLES:
            with self.subTest(role=role):
                result, state = self.execute(role, fail_allocation=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call["tool"] == "cmake" or "clone" in call["args"] for call in state["calls"]))
                self.assertIn("allocation failed", result.stderr)

    def test_each_codeql_cmake_step_requires_the_private_working_directory(self):
        text = self.workflow("codeql")
        for step in self.steps("codeql", text):
            fields, _ = self.quality.workflow_step_fields(step)
            if fields.get("name") not in self.CONSUMERS["codeql"]:
                continue
            self.assertIn("working-directory", fields)
            original = "\n".join(step)
            mutated = "\n".join(line for line in step if "working-directory:" not in line)
            with self.subTest(step=fields["name"]):
                result, _ = self.execute("codeql", text.replace(original, mutated, 1))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("MODEL", result.stderr)

    def test_changed_scope_static_tools_use_the_same_private_build(self):
        result, state = self.execute("static", static_mode="changed-strict")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        tidy = [call for call in state["calls"] if call["tool"] == "python3"
                and pathlib.Path(call["args"][0]).name == "run_changed_clang_tidy.py"]
        self.assertEqual(len(tidy), 1)
        self.assertTrue(any(call["tool"] == "cppcheck" and "--error-exitcode=1" in call["args"]
                            for call in state["calls"]))
        text = self.workflow("static")
        step = next(step for step in self.steps("static", text)
                    if self.quality.workflow_step_fields(step)[0].get("name") == "Run clang-tidy")
        old = "\n".join(step)
        new = old.replace('--build-dir "$KLOGG_BUILD_ROOT"', '--build-dir "$GITHUB_WORKSPACE/build_root"')
        self.assertNotEqual(old, new)
        result, _ = self.execute("static", text.replace(old, new, 1), static_mode="changed-strict")
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "build directory|clang-tidy used another build")

    def test_coverage_filters_private_source_and_transports_reports(self):
        result, state = self.execute("coverage")
        summary = self.workspace / "coverage_report/summary.txt"
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr
                         + (summary.read_text() if summary.exists() else ""))
        self.assertEqual(len(state["coverage_selections"]), 2)
        for selection in state["coverage_selections"]:
            self.assertEqual(selection["cwd"], state["source"])
            self.assertTrue(selection["first_party"])
            self.assertFalse(selection["original_checkout"])
            self.assertFalse(selection["vendor"])
        report = self.workspace / "coverage_report"
        staged = pathlib.Path(state["source"]) / "coverage_report"
        self.assertEqual({path.name for path in report.iterdir()}, {path.name for path in staged.iterdir()})
        for path in staged.iterdir():
            self.assertEqual((report / path.name).read_bytes(), path.read_bytes())

    def test_each_gcovr_call_requires_private_source_filter_context(self):
        # Mutate only the fixture copy of the real helper, not the workflow or
        # its invocation boundary. Relative gcovr filters must use private cwd.
        mutations = (
            ('build], cwd=workspace, env=environment).stdout',
             'build], cwd=pathlib.Path(environment["GITHUB_WORKSPACE"]), env=environment).stdout'),
            ('report / "coverage.json", build], cwd=workspace, env=environment)',
             'report / "coverage.json", build], cwd=pathlib.Path(environment["GITHUB_WORKSPACE"]), env=environment)'),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation[0]):
                result, state = self.execute("coverage", helper_mutation=mutation)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(state.get("ratcheted"))
                self.assertIn("reporting working directory", result.stderr)

    def test_coverage_transport_cannot_reuse_stale_reports_or_hide_copy_failure(self):
        text = self.workflow("coverage")
        transport = 'cp -R "$KLOGG_WORKSPACE/coverage_report" "$GITHUB_WORKSPACE/"'
        self.assertIn(transport, text)
        for replacement in (":", 'cp -R "$KLOGG_WORKSPACE/coverage_report" "$GITHUB_WORKSPACE/missing-parent/destination/"'):
            with self.subTest(transport=replacement):
                stale = self.workspace / "coverage_report"
                stale.mkdir(exist_ok=True)
                for name in ("summary.txt", "coverage.json", "index.html"):
                    (stale / name).write_text("stale report from a previous scenario\n")
                result, state = self.execute("coverage", text.replace(transport, replacement, 1))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(stale.exists())
                self.assertFalse(state.get("ratcheted"))
                staged = pathlib.Path(state["source"]) / "coverage_report"
                self.assertIn("100.0%", (staged / "summary.txt").read_text())
                self.assertTrue(all(selection["first_party"] for selection in state["coverage_selections"]))

    def test_coverage_report_modes_and_summary_are_required(self):
        for argument in ('"--print-summary", ', '"--html-details", ', '"--html-self-contained", ',
                         '"--html-title", "Klogg line/branch coverage",', '"--json", ', '"--json-pretty", '):
            with self.subTest(missing=argument):
                result, state = self.execute("coverage", helper_mutation=(argument, ""))
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr, "HTML report mode|JSON report mode")
                self.assertFalse(state.get("ratcheted"))

    def test_actual_ratchet_rejects_corrupted_synthetic_summaries(self):
        text = self.workflow("coverage")
        consumer = 'sh scripts/run_linux_analysis.sh coverage-ratchet'
        self.assertIn(consumer, text)
        corruptions = ((":", "missing a parseable 'lines'"),
                       ("printf '%s\\n' 'not a percentage'", "missing a parseable 'lines'"),
                       ("printf '%s\\n' 'lines: 100.0%' 'branches: invalid'", "missing a parseable 'branches'"),
                       ("printf '%s\\n' 'lines: 0.0%' 'branches: 100.0%'", "below the ratchet floor"))
        for write, message in corruptions:
            with self.subTest(summary=write):
                injected = write + " > coverage_report/summary.txt\n          " + consumer
                result, state = self.execute("coverage", text.replace(consumer, injected, 1))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(state.get("ratcheted"))

    def test_coverage_ratchet_preserves_original_context_for_each_event(self):
        for event in ("pull_request", "push", "workflow_dispatch"):
            with self.subTest(event=event):
                result, state = self.execute("coverage", event=event)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(state["ratcheted"])
                ratchet = next(call for call in state["calls"] if call["tool"] == "python3"
                               and pathlib.Path(call["args"][0]).name == "enforce_coverage_ratchet.py")
                self.assertEqual(ratchet["cwd"], str(self.workspace))
                self.assertEqual(ratchet["args"][ratchet["args"].index("--base-sha") + 1], self.base)
        result, state = self.execute("coverage", event="unknown-event")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported coverage ratchet event", result.stdout + result.stderr)
        self.assertFalse(state.get("ratcheted"))

    def test_tool_failures_stop_before_successful_consumers(self):
        for role, tools in (("static", ("cmake", "clang-tidy", "cppcheck")),
                            ("coverage", ("cmake", "ctest", "gcovr"))):
            for tool in tools:
                with self.subTest(role=role, tool=tool):
                    result, state = self.execute(role, fail_tool=tool)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("injected " + tool + " failure", result.stderr)
                    self.assertFalse(state.get("ratcheted"))

    def test_coverage_cache_alias_is_bound_to_the_original_checkout(self):
        text = self.workflow("coverage")
        source = 'source_root=$(cd "$GITHUB_WORKSPACE" && pwd -P)'
        cache = r'''printf 'KLOGG_CPM_SOURCE_CACHE=%s/cpm_cache\n' "$source_root" >> "$GITHUB_ENV"'''
        for old, new in ((source, 'source_root="$private_parent/source"'),
                         (cache, cache.replace('"$source_root"', '"$private_parent/source"')),
                         (cache, "# cache binding omitted"), (source, "# " + source), (cache, "# " + cache)):
            with self.subTest(binding=new):
                self.assertIn(old, text)
                result, state = self.execute("coverage", text.replace(old, new, 1))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(state.get("ratcheted"))

    def test_consumer_and_parent_mutations_fail_observably(self):
        mutations = {
            "static": [('KLOGG_BUILD_ROOT=%s/build_root', 'KLOGG_BUILD_ROOT=build_root'),
                       ('--build-dir "$KLOGG_BUILD_ROOT"', '--build-dir "$GITHUB_WORKSPACE/build_root"')],
            "coverage": [('git -C "$private_parent/source" checkout --detach "$source_head"',
                          'git -C "$private_parent/source" checkout --detach main'),
                         ('--no-hardlinks --no-checkout', '--no-checkout'),
                         ('"$source_root/.git"', '"$source_root"'),
                         ('--workspace "$KLOGG_WORKSPACE"', '--workspace "$GITHUB_WORKSPACE"'),
                         ('--build-dir "$KLOGG_BUILD_ROOT"', '--build-dir "$GITHUB_WORKSPACE/build_root"'),
                         ('--cpm-source-cache "$KLOGG_CPM_SOURCE_CACHE"', '--cpm-source-cache "$KLOGG_WORKSPACE/cpm_cache"'),
                         ('--report-dir "$KLOGG_WORKSPACE/coverage_report"', '--report-dir "$KLOGG_BUILD_ROOT/coverage_report"'),
                         ('--workspace "$GITHUB_WORKSPACE"', '--workspace "$KLOGG_WORKSPACE"'),
                         ('--report-dir "$GITHUB_WORKSPACE/coverage_report"', '--report-dir "$KLOGG_BUILD_ROOT/coverage_report"'),
                         ('sh scripts/run_linux_analysis.sh configure', 'sh "$KLOGG_WORKSPACE/scripts/run_linux_analysis.sh" configure'),
                         ('path: coverage_report/', 'path: build_root/')],
        }
        for role, changes in mutations.items():
            text = self.workflow(role)
            for old, new in changes:
                self.assertIn(old, text)
                offset = text.find(old)
                while offset >= 0:
                    with self.subTest(role=role, mutation=new, occurrence=offset):
                        mutated = text[:offset] + new + text[offset + len(old):]
                        result, _ = self.execute(role, mutated)
                        self.assertNotEqual(result.returncode, 0)
                    offset = text.find(old, offset + len(old))
        for role in self.ROLES:
            text = self.workflow(role)
            marker = 'private_parent=$(mktemp -d /tmp/klogg-analysis.XXXXXXXX)'
            self.assertIn(marker, text)
            result, _ = self.execute(role, text.replace(marker, marker + '\n          rm -rf "$private_parent"', 1))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("retain the authority-bearing", result.stderr)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--analysis-model":
        try:
            model_tool(sys.argv[2], sys.argv[3:])
        except (ValueError, OSError) as error:
            sys.stderr.write(str(error) + "\n")
            sys.exit(86)
    else:
        unittest.main()
