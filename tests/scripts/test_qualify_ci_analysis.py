"""Deterministic real-command analysis contracts; no compilation or downloads."""
from __future__ import annotations

import contextlib
import copy
import csv
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_environment as ci
import qualify_ci_analysis as analysis

BASE = "a" * 40
HEAD = "b" * 40
FAMILY = "noble-qt693-analysis"


def write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")


class AnalysisRoleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="analysis-role-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.workspace = self.root / "workspace"
        self.work = self.root / "work"
        self.output = self.root / "result"
        self.workspace.mkdir()
        self.work.mkdir()
        profiles = ci.load_json(ROOT / "ci/environments/profiles.json")
        write_json(self.workspace / "ci/environments/profiles.json", profiles)
        (self.workspace / "src/app").mkdir(parents=True)
        (self.workspace / "src/app/main.cpp").write_text("int main() { return 0; }\n", encoding="ascii")
        self.commands = []
        self.environments = []
        self.changed = "src/app/main.cpp\0"
        self.dry_run = "[1/3] Building CXX object src/app/CMakeFiles/klogg.dir/main.cpp.o\n"
        self.missing_census = None
        self.fail_token = None
        self.codeql = self.root / "private-codeql"
        for name in ("cpp-all", "cpp-queries"):
            pack = self.codeql / "qlpacks/codeql" / name / "9.8.7"
            pack.mkdir(parents=True)
            (pack / "qlpack.yml").write_text("name: codeql/" + name + "\nversion: 9.8.7\n", encoding="ascii")
            if name == "cpp-queries":
                (pack / "codeql-suites").mkdir()
                (pack / "codeql-suites/cpp-code-scanning.qls").write_text("- queries: .\n", encoding="ascii")
        (self.codeql / "codeql").write_bytes(b"synthetic proprietary executable; must never be output")
        self.expected_sources = []

    @staticmethod
    def option(command, name):
        for index, value in enumerate(command):
            if value == name:
                return command[index + 1]
            if value.startswith(name + "="):
                return value[len(name) + 1:]
        raise AssertionError("missing option " + name + " in " + repr(command))

    def compile_database(self, build):
        units = [
            (self.workspace / "src/app/main.cpp", "src/app/CMakeFiles/klogg.dir/main.cpp.o"),
            (build / "src/app/klogg_autogen/mocs_compilation.cpp", "src/app/CMakeFiles/klogg.dir/klogg_autogen/mocs_compilation.cpp.o"),
            (build / "src/app/klogg_common_resources_autogen/ABC/qrc_klogg.cpp", "src/app/CMakeFiles/klogg_common_resources.dir/klogg_common_resources_autogen/ABC/qrc_klogg.cpp.o"),
            (self.workspace / "3rdparty/dependency/vendor.cpp", "3rdparty/CMakeFiles/dependency.dir/vendor.cpp.o"),
        ]
        self.expected_sources = [str(source) for source, _ in units[:3]]
        write_json(build / "compile_commands.json", [{"directory": str(build), "file": str(source), "output": output,
                                                     "arguments": ["g++-13", "-c", str(source), "-o", output]} for source, output in units])
        self.objects = [output for _, output in units]

    def runner(self, command, **kwargs):
        command = [str(value) for value in command]
        self.commands.append(command)
        self.environments.append(kwargs.get("env", {}))
        if self.fail_token and any(self.fail_token in value for value in command):
            raise subprocess.CalledProcessError(19, command, stderr="synthetic tool failure")
        stdout = ""
        if command[0] == "git":
            if "rev-parse" in command:
                stdout = HEAD + "\n"
            elif "diff" in command and "--name-only" in command:
                stdout = self.changed
        elif command[0] == "cmake" and "-S" in command:
            build = pathlib.Path(self.option(command, "-B"))
            build.mkdir(parents=True, exist_ok=True)
            self.compile_database(build)
            (build / ".ninja_log").write_text("# ninja log v5\n", encoding="ascii")
        elif command[0] == "cmake" and "klogg_codeql_thirdparty" in command:
            build = pathlib.Path(self.option(command, "--build"))
            (build / ".ninja_log").write_text("# ninja log v5\n0\t1\t0\t" + self.objects[-1] + "\tdeadbeef\n", encoding="ascii")
        elif command[0] == "cmake" and "-n" in command:
            stdout = self.dry_run
        elif pathlib.Path(command[0]).name == "codeql":
            if command[1] == "version":
                stdout = json.dumps({"version": "2.26.4"})
            elif command[1:3] == ["database", "create"]:
                build = self.work / "build"
                with (build / ".ninja_log").open("a", encoding="ascii") as stream:
                    for output in self.objects[:3]:
                        stream.write("2\t3\t0\t" + output + "\tfeedbeef\n")
            elif command[1:3] == ["bqrs", "decode"]:
                path = pathlib.Path(self.option(command, "--output"))
                rows = [path for path in self.expected_sources if not self.missing_census or self.missing_census not in path]
                with path.open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.writer(stream)
                    writer.writerows([[value] for value in rows])
            elif command[1:3] == ["database", "analyze"]:
                write_json(pathlib.Path(self.option(command, "--output")), {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}}, "results": []}]})
        elif command[0] == "gcovr":
            path = pathlib.Path(self.option(command, "-o"))
            if "--json" in command:
                write_json(path, {"gcovr/format_version": "0.6", "files": []})
            else:
                path.write_text("<html>coverage fixture</html>", encoding="ascii")
                stdout = "lines: 99.0%\nbranches: 98.0%\n"
        return subprocess.CompletedProcess(command, 0, stdout, "")

    def run_role(self, role):
        return analysis.run_analysis(role, self.workspace, self.work, self.output, BASE,
                                     codeql_root=self.codeql if role == "codeql" else None,
                                     expected_codeql_version="2.26.4" if role == "codeql" else None,
                                     runner=self.runner, jobs=2)

    def test_role_failure_cannot_emit_success_evidence(self):
        self.fail_token = "cmake"
        with self.assertRaises(ci.ContractError):
            self.run_role("coverage")
        self.assertFalse((self.output / "analysis-evidence.json").exists())

    def test_static_runs_full_tools_and_existing_changed_scope_strict_gates(self):
        result = self.run_role("static")
        self.assertEqual(result["role"], "static")
        self.assertEqual(result["result"], "passed")
        self.assertGreater(result["checks"]["full_first_party_units"], 0)
        self.assertTrue(any(command[0] == "clang-tidy" for command in self.commands))
        changed = next(command for command in self.commands if any(value.endswith("run_changed_clang_tidy.py") for value in command))
        self.assertIn(BASE, changed)
        self.assertNotIn("--fast", changed)
        cppcheck = [command for command in self.commands if command[0] == "cppcheck"]
        self.assertEqual(len(cppcheck), 2)
        self.assertNotIn("--error-exitcode=1", cppcheck[0])
        self.assertIn("--error-exitcode=1", cppcheck[1])
        for command in cppcheck:
            self.assertIn("--enable=warning,style", command)
            self.assertTrue(any(value.endswith("tests/cppcheck_suppressions.txt") for value in command))
        self.assertFalse(any("codeql" == pathlib.Path(command[0]).name for command in self.commands))

    def test_missing_changed_source_compile_command_is_not_silently_skipped(self):
        self.changed = "src/platform/missing.cpp\0"
        with self.assertRaisesRegex(ci.ContractError, "missing compile command"):
            self.run_role("static")
        self.assertFalse((self.output / "analysis-evidence.json").exists())

    def test_tool_failure_preserves_diagnostic_output(self):
        self.fail_token = "cmake"
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics), self.assertRaises(ci.ContractError):
            self.run_role("coverage")
        self.assertIn("synthetic tool failure", diagnostics.getvalue())

    def test_static_infrastructure_only_diff_still_runs_full_analysis(self):
        self.changed = ""
        result = self.run_role("static")
        self.assertGreater(result["checks"]["full_first_party_units"], 0)
        self.assertEqual(result["checks"]["changed_source_units"], 0)
        self.assertTrue(any(command[0] == "clang-tidy" for command in self.commands))
        self.assertTrue(any(command[0] == "cppcheck" for command in self.commands))

    def test_changed_headers_keep_full_cppcheck_sweep_strict(self):
        self.changed = "src/shared.h\0"
        result = self.run_role("static")
        self.assertEqual(result["checks"]["changed_headers"], 1)
        self.assertEqual(result["checks"]["changed_source_units"], result["checks"]["full_first_party_units"])
        strict = [command for command in self.commands if command[0] == "cppcheck" and "--error-exitcode=1" in command]
        self.assertEqual(len(strict), 1)

    def test_coverage_builds_all_targets_runs_all_ctest_and_current_ratchet(self):
        result = self.run_role("coverage")
        configure = next(command for command in self.commands if command[0] == "cmake" and "-S" in command)
        for option in ("-DCMAKE_BUILD_TYPE=Debug", "-DENABLE_COVERAGE=ON", "-DKLOGG_USE_VECTORSCAN=OFF", "-DKLOGG_OVERRIDE_MALLOC=OFF"):
            self.assertIn(option, configure)
        build = next(command for command in self.commands if command[0] == "cmake" and "--build" in command)
        for target in ("klogg", "klogg_grep", "klogg_test_build"):
            self.assertIn(target, build)
        ctest = next(command for command in self.commands if command[0] == "ctest")
        self.assertIn("--output-on-failure", ctest)
        self.assertIn("--parallel", ctest)
        self.assertNotIn("-R", ctest)
        reports = [command for command in self.commands if command[0] == "gcovr"]
        self.assertEqual(len(reports), 2)
        for command in reports:
            self.assertIn("negative_hits.warn_once_per_file", command)
            self.assertIn("^src/", command)
        ratchet = next(command for command in self.commands if any(value.endswith("enforce_coverage_ratchet.py") for value in command))
        self.assertIn("--base-sha", ratchet)
        self.assertIn(BASE, ratchet)
        self.assertEqual(result["checks"]["ratchet_base"], BASE)
        self.assertTrue((self.output / "coverage-summary.txt").is_file())

    def test_missing_explicit_disconnected_policy_fails_before_configure(self):
        path = self.workspace / "ci/environments/profiles.json"
        document = ci.load_json(path)
        options = document["families"][FAMILY]["coverage"]["configuration"]["cmake_options"]
        options[:] = [value for value in options if not value.startswith("-DFETCHCONTENT_FULLY_DISCONNECTED=")]
        write_json(path, document)
        with self.assertRaisesRegex(ci.ContractError, "explicit disconnected"):
            self.run_role("coverage")
        self.assertFalse(any(command[0] == "cmake" for command in self.commands))

    def test_coverage_ratchet_failure_never_produces_passed_evidence(self):
        self.fail_token = "enforce_coverage_ratchet.py"
        with self.assertRaises(ci.ContractError):
            self.run_role("coverage")
        self.assertFalse((self.output / "analysis-evidence.json").exists())

    def test_codeql_prebuild_guard_real_trace_census_and_bundled_analysis_order(self):
        result = self.run_role("codeql")
        commands = self.commands
        prebuild = next(index for index, command in enumerate(commands) if "klogg_codeql_thirdparty" in command)
        guard = next(index for index, command in enumerate(commands) if "-n" in command)
        create = next(index for index, command in enumerate(commands) if command[1:3] == ["database", "create"])
        analyze = next(index for index, command in enumerate(commands) if command[1:3] == ["database", "analyze"])
        self.assertLess(prebuild, guard)
        self.assertLess(guard, create)
        self.assertLess(create, analyze)
        create_command = commands[create]
        self.assertIn("--language=c-cpp", create_command)
        self.assertIn("codeql-trace-build", self.option(create_command, "--command"))
        self.assertNotIn("--overwrite", create_command)
        self.assertIn("--no-download", commands[analyze])
        self.assertTrue(any(value.endswith("cpp-code-scanning.qls") for value in commands[analyze]))
        self.assertTrue(any(command[1:3] == ["query", "run"] for command in commands))
        self.assertEqual(result["checks"]["census"]["matched_translation_units"], 3)
        self.assertEqual(result["checks"]["census"]["moc_units"], 1)
        self.assertEqual(result["checks"]["census"]["rcc_units"], 1)
        self.assertEqual(result["checks"]["codeql_version"], "2.26.4")
        self.assertEqual(result["checks"]["query_pack"]["version"], "9.8.7")
        self.assertEqual({path.name for path in self.output.iterdir()}, {"analysis-evidence.json", "codeql.sarif"})
        self.assertTrue(all(env.get("CCACHE_DISABLE") == "1" for env in self.environments))

    def test_codeql_refuses_vendored_plan_or_missing_moc_rcc_extraction(self):
        self.dry_run = "Building CXX object cpm_cache/vendor/CMakeFiles/vendor.dir/file.cpp.o\n"
        with self.assertRaisesRegex(ci.ContractError, "vendored"):
            self.run_role("codeql")
        self.assertFalse(any(command[1:3] == ["database", "create"] for command in self.commands))

    def test_codeql_census_compile_database_is_an_explicit_profile_requirement(self):
        path = self.workspace / "ci/environments/profiles.json"
        document = ci.load_json(path)
        options = document["families"][FAMILY]["codeql"]["configuration"]["cmake_options"]
        options[:] = [value for value in options if not value.startswith("-DCMAKE_EXPORT_COMPILE_COMMANDS=")]
        write_json(path, document)
        with self.assertRaisesRegex(ci.ContractError, "explicit compile database"):
            self.run_role("codeql")
        self.assertFalse(any(command[0] == "cmake" for command in self.commands))

    def test_codeql_missing_generated_extraction_is_not_a_success_marker(self):
        self.missing_census = "qrc_"
        with self.assertRaisesRegex(ci.ContractError, "census"):
            self.run_role("codeql")
        self.assertFalse((self.output / "analysis-evidence.json").exists())
        self.assertFalse(any(command[1:3] == ["database", "analyze"] for command in self.commands))

    def test_codeql_analysis_failure_cannot_pass_after_successful_trace(self):
        self.fail_token = "--format=sarif-latest"
        with self.assertRaises(ci.ContractError):
            self.run_role("codeql")
        self.assertTrue(any(command[1:3] == ["database", "create"] for command in self.commands))
        self.assertFalse((self.output / "analysis-evidence.json").exists())
        self.assertFalse((self.output / "codeql.sarif").exists())

    def test_existing_build_or_database_is_rejected_before_tools_run(self):
        for name in ("build", "codeql-database"):
            stale = self.work / name
            stale.mkdir()
            with self.subTest(name=name), self.assertRaises(ci.ContractError):
                self.run_role("codeql")
            stale.rmdir()
        self.assertEqual(self.commands, [])

    def test_codeql_requires_bundled_packs(self):
        (self.codeql / "qlpacks/codeql/cpp-queries/9.8.7/codeql-suites/cpp-code-scanning.qls").unlink()
        with self.assertRaises(ci.ContractError):
            self.run_role("codeql")
        self.assertFalse(any(command[1:3] == ["database", "create"] for command in self.commands))

    def test_resource_ownership_exception_does_not_promote_unknown_targets(self):
        build = self.work / "build"
        build.mkdir()
        self.compile_database(build)
        database = analysis.read_json(build / "compile_commands.json")
        for key in ("file", "output"):
            database[2][key] = database[2][key].replace("klogg_common_resources", "unknown_resources")
        write_json(build / "compile_commands.json", database)
        owners, _ = analysis.compilation_map(self.workspace, build)
        self.assertEqual(owners[str((build / database[2]["output"]).resolve())], "unknown")

    def test_result_directory_cannot_include_private_database_and_build_tree(self):
        with self.assertRaisesRegex(ci.ContractError, "private analysis work"):
            analysis.run_analysis("codeql", self.workspace, self.work, self.work, BASE,
                                  codeql_root=self.codeql, expected_codeql_version="2.26.4", runner=self.runner, jobs=2)
        self.assertEqual(self.commands, [])

    def test_malformed_sarif_tool_data_fails_as_contract_error(self):
        sarif = self.root / "bad.sarif"
        write_json(sarif, {"version": "2.1.0", "runs": [{"tool": []}]})
        with self.assertRaises(ci.ContractError):
            analysis.check_sarif(sarif)

    def test_shared_shell_phases_are_available_without_running_tools(self):
        script = ROOT / "scripts/run_linux_analysis.sh"
        result = subprocess.run(["sh", str(script), "--help"], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        for phase in ("configure", "codeql-prebuild", "codeql-trace-build", "qualify"):
            self.assertIn(phase, result.stdout)


class AnalysisCandidateBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="analysis-boundary-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.candidate_root = self.root / "candidate"
        self.candidate_root.mkdir()
        self.output = self.root / "result"
        self.document = ci.load_json(ROOT / "ci/environments/profiles.json")
        self.document["families"] = {FAMILY: self.document["families"][FAMILY]}
        paths = set(self.document["verification_files"])
        for profile in self.document["families"][FAMILY].values():
            paths.update(profile["verification_files"])
        for relative in paths:
            path = self.workspace / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic verification file\n")
        write_json(self.workspace / "ci/environments/profiles.json", self.document)
        recipe = self.workspace / "docker/analysis/Dockerfile"
        recipe.parent.mkdir(parents=True)
        recipe.write_bytes(b"synthetic recipe bytes\n")
        catalog = {"schema_version": 1, "registry": ci.REGISTRY, "families": {FAMILY: {
            "platform": "linux/amd64", "dockerfile": "docker/analysis/Dockerfile", "context": "docker/analysis",
            "recipe_files": ["docker/analysis/Dockerfile"], "profiles": ["static", "coverage", "codeql"]}}}
        write_json(self.workspace / "ci/environments/recipes.json", catalog)
        digest = lambda text: "sha256:" + hashlib.sha256(text.encode("ascii")).hexdigest()
        image = {"schema_version": 1, "platform": "linux/amd64", "archive_sha256": hashlib.sha256(b"OCI fixture").hexdigest(),
                 "manifest_digest": digest("manifest"), "config_digest": digest("config"), "diff_ids": [digest("diff")], "layer_digests": [digest("layer")]}
        inputs = {"schema_version": 1, "family": FAMILY, "fixture": "resolved test inputs"}
        self.candidate = {"schema_version": 1, "kind": "candidate", "family": FAMILY,
                          "recipe_digest": ci.recipe_identity(catalog, FAMILY, self.workspace), "input_digest": ci.input_identity(inputs),
                          "image": image, "source": {"repository": "ZEACENT/klogg", "sha": HEAD, "ref": "refs/heads/master",
                            "workflow": ".github/workflows/ci-environments.yml", "run_id": 123, "run_attempt": 1}}
        write_json(self.candidate_root / "candidate.json", self.candidate)
        write_json(self.candidate_root / "inputs.json", inputs)
        self.loaded = {"Id": image["config_digest"], "Architecture": "amd64", "Os": "linux", "RootFS": {"Type": "layers", "Layers": image["diff_ids"]}}
        self.bundle = b"synthetic official-bundle bytes, never a production lock"
        self.materials = {"codeql": [{"name": "codeql-bundle", "url": "https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.26.4/codeql-bundle-linux64.tar.gz",
                                      "sha256": hashlib.sha256(self.bundle).hexdigest()}]}
        self.commands = []
        self.downloads = []
        self.extractions = []
        self.head = HEAD
        self.fail_run = False
        self.forbidden_output = False

    def downloader(self, url, path):
        self.downloads.append(url)
        path.write_bytes(self.bundle)

    def extractor(self, archive, destination):
        self.extractions.append(destination)
        destination.mkdir()
        (destination / "codeql").write_bytes(b"synthetic proprietary binary")

    def runner(self, command, **kwargs):
        self.commands.append(command)
        stdout = ""
        if command[0] == "git" and "rev-parse" in command:
            stdout = self.head + "\n"
        elif command[:3] == ["docker", "image", "inspect"]:
            stdout = json.dumps([self.loaded])
        elif command[:2] == ["docker", "run"]:
            if self.fail_run:
                raise subprocess.CalledProcessError(1, command, stderr="real role command failed")
            mounts = [next(csv.reader([command[index + 1]])) for index, value in enumerate(command[:-1]) if value == "--mount"]
            result = next(pathlib.Path(next(field.split("=", 1)[1] for field in mount if field.startswith("source=")))
                          for mount in mounts if "target=/klogg-analysis-result" in mount)
            role = command[command.index("--role") + 1]
            write_json(result / "analysis-evidence.json", {"schema_version": 1, "kind": "analysis-evidence", "role": role,
                       "result": "passed", "analysis_base_sha": BASE, "checks": {"fixture_successful_role_commands": True}})
            if role == "codeql":
                write_json(result / "codeql.sarif", {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}}, "results": []}]})
            elif role == "coverage":
                (result / "coverage-summary.txt").write_text("lines: 99%\nbranches: 98%\n", encoding="ascii")
                write_json(result / "coverage.json", {"files": []})
            if self.forbidden_output:
                (result / "codeql-binary").write_bytes(b"must never leave private scratch")
        return subprocess.CompletedProcess(command, 0, stdout, "")

    def qualify(self, role, **kwargs):
        return analysis.qualify(role, self.candidate_root, self.workspace, BASE, self.materials,
                                kwargs.pop("output", self.output), runner=kwargs.pop("runner", self.runner),
                                downloader=kwargs.pop("downloader", self.downloader), extractor=self.extractor, **kwargs)

    def test_static_and_coverage_never_acquire_or_mount_codeql(self):
        for role in ("static", "coverage"):
            result = self.qualify(role, output=self.root / role)
            self.assertEqual(result["kind"], "analysis-evidence")
            self.assertEqual(result["source_sha"], HEAD)
            self.assertNotIn("candidate_artifact_id", result)
            self.assertRegex(result["policy_digest"], r"^sha256:[0-9a-f]{64}$")
        self.assertEqual(self.downloads, [])
        self.assertEqual(self.extractions, [])
        for command in self.commands:
            if command[:2] == ["docker", "run"]:
                self.assertIn("--pull=never", command)
                self.assertIn("--network=none", command)
                self.assertIn(self.candidate["image"]["config_digest"], command)
                self.assertNotIn("--privileged", command)
                self.assertFalse(any("target=/klogg-codeql" in value for value in command))

    def test_codeql_download_is_hash_locked_private_and_only_sarif_evidence_escape(self):
        result = self.qualify("codeql")
        self.assertEqual(self.downloads, [self.materials["codeql"][0]["url"]])
        self.assertEqual(result["role_materials"], self.materials["codeql"])
        self.assertEqual(set(result["artifacts"]), {"codeql.sarif"})
        self.assertEqual({path.name for path in self.output.iterdir()}, {"analysis-evidence.json", "codeql.sarif"})
        command = next(command for command in self.commands if command[:2] == ["docker", "run"])
        self.assertTrue(any("target=/klogg-codeql,readonly" in value for value in command))
        self.assertIn("CCACHE_DISABLE=1", command)
        self.assertTrue(all(not path.exists() for path in self.extractions))

    def test_wrong_image_or_checkout_fails_before_real_role_or_download(self):
        self.head = "c" * 40
        with self.assertRaisesRegex(ci.ContractError, "source SHA"):
            self.qualify("codeql")
        self.head = HEAD
        self.loaded["Id"] = "sha256:" + "c" * 64
        with self.assertRaises(ci.ContractError):
            self.qualify("codeql")
        self.assertEqual(self.downloads, [])
        self.assertFalse(any(command[:2] == ["docker", "run"] for command in self.commands))

    def test_bad_download_cannot_be_extracted_or_executed(self):
        def wrong(url, path):
            path.write_bytes(b"tampered bundle")
        with self.assertRaisesRegex(ci.ContractError, "checksum mismatch"):
            self.qualify("codeql", downloader=wrong)
        self.assertEqual(self.extractions, [])
        self.assertFalse(any(command[:2] == ["docker", "run"] for command in self.commands))
        self.assertFalse(self.output.exists())

    def test_failed_role_and_forbidden_payload_do_not_publish_evidence(self):
        self.fail_run = True
        with self.assertRaises(ci.ContractError):
            self.qualify("static")
        self.assertFalse(self.output.exists())
        self.fail_run = False
        self.forbidden_output = True
        with self.assertRaisesRegex(ci.ContractError, "forbidden payloads"):
            self.qualify("codeql")
        self.assertFalse(self.output.exists())

    def test_checkout_head_changed_during_role_cannot_be_qualified(self):
        def changed(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[:2] == ["docker", "run"]:
                self.head = "c" * 40
            return result
        with self.assertRaisesRegex(ci.ContractError, "source SHA changed"):
            self.qualify("static", runner=changed)
        self.assertFalse(self.output.exists())

    def test_existing_output_is_not_reused(self):
        self.qualify("static")
        count = len(self.commands)
        with self.assertRaises(ci.ContractError):
            self.qualify("static")
        self.assertEqual(len(self.commands), count)


if __name__ == "__main__":
    unittest.main()
