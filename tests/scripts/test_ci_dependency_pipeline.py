"""A trusted workflow gate must not promote self-reported offline candidates."""

import ast
import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import textwrap
import types
import unittest
from unittest import mock

from test_ci_environment_workflow import QUALITY, event_expression

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_catalog as catalog
from ci_dependency_core import ADB_RUNTIME, package_core
import ci_dependency_pipeline as pipeline


def sha(data):
    return hashlib.sha256(data).hexdigest()


class DependencyPipelineTest(unittest.TestCase):
    JOB_IDS = {
        "adb-linux-x86_64": "BuildAdbLinuxX64",
        "adb-linux-arm64": "BuildAdbLinuxArm64",
        "adb-windows-x86_64": "BuildAdbWindowsX64",
        "adb-macos-x86_64": "BuildAdbMacX64",
        "adb-macos-arm64": "BuildAdbMacArm64",
        "ios-x86_64": "BuildIosNativeX64",
        "ios-arm64": "BuildIosNativeArm64",
    }

    @classmethod
    def setUpClass(cls):
        cls.catalog_bytes = (ROOT / "ci/dependencies/catalog.json").read_bytes()
        cls.catalog = json.loads(cls.catalog_bytes)
        cls.environment = {target_id: {"runner": row["runner"],
                                       **({"toolchain": row["toolchain"]} if "toolchain" in row else {})}
                           for target_id, row in cls.catalog["targets"].items()}
        cls.identities = pipeline.compute_identities(ROOT, cls.catalog)
        cls.source = {
            "repository": "ZEACENT/klogg", "workflow": ".github/workflows/ci-build.yml",
            "event_name": "workflow_dispatch", "sha": "a" * 40,
            "ref": "refs/heads/master", "run_id": 1234,
            "run_attempt": 2,
        }
        cls.builder = []
        cls.artifacts = []
        cls.downloads = {}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for index, (target_id, row) in enumerate(sorted(cls.catalog["targets"].items()), 1):
                stage = root / target_id
                if row["component"] == "adb-helper":
                    helper = "adb.exe" if row["target"].startswith("windows-") else "adb"
                    closure = [(helper, 0o755)] + [(name, 0o644) for name in ADB_RUNTIME[row["target"]]]
                    for name, mode in closure:
                        path = stage / "helpers" / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes((target_id + name).encode())
                        path.chmod(mode)
                else:
                    path = stage / "lib/libfixture.dylib"
                    path.parent.mkdir(parents=True)
                    path.write_bytes(target_id.encode())
                    path.chmod(0o755)
                archive = root / row["archive_name"]
                info = package_core(stage, archive, component=row["component"],
                                    target=row["target"],
                                    core_identity=cls.identities[target_id]["core_identity"])
                candidate = {
                    "schema_version": 1, "kind": "native-dependency-core-candidate",
                    "publication_status": "candidate-only", "catalog_target": target_id,
                    "catalog_sha256": sha(cls.catalog_bytes), "component": row["component"],
                    "target": row["target"], "runner": row["runner"],
                    **cls.identities[target_id],
                    "archive": {"name": row["archive_name"], **info},
                }
                if row["component"] == "adb-helper":
                    candidate["qualification"] = {
                        "legacy_package_receipt_sha256": "b" * 64,
                        "rechecked_package_receipt_sha256": "c" * 64,
                    }
                raw = (json.dumps(candidate, sort_keys=True) + "\n").encode()
                artifact_id = index + 100
                cls.downloads[artifact_id] = {"candidate": raw, "archive": archive.read_bytes()}
                cls.builder.append({
                    "job_id": cls.JOB_IDS[target_id], "conclusion": "success",
                    "outputs": {"artifact_id": str(artifact_id),
                                "candidate_sha256": sha(raw), "archive_sha256": info["sha256"]},
                })
                cls.artifacts.append({
                    "id": artifact_id,
                    "name": f"native-core-candidate-{target_id}-1234-2",
                    "expired": False,
                    "workflow_run": {"id": 1234, "head_sha": "a" * 40,
                                     "head_branch": "master"},
                })

    def inputs(self):
        return (copy.deepcopy(self.source), copy.deepcopy(self.catalog),
                self.catalog_bytes, copy.deepcopy(self.builder),
                copy.deepcopy(self.artifacts), copy.deepcopy(self.downloads),
                copy.deepcopy(self.identities))

    def qualify(self, values=None, **kwargs):
        kwargs.setdefault("trusted_environment", copy.deepcopy(self.environment))
        return pipeline.qualify_candidates(*(self.inputs() if values is None else values), **kwargs)

    def test_seven_successful_immutable_candidates_yield_deterministic_unpublished_receipts(self):
        first = self.qualify()
        self.assertEqual(first, self.qualify())
        self.assertEqual(set(first), set(catalog.TARGETS))
        for target_id, receipt in first.items():
            self.assertEqual(receipt["kind"], "native-dependency-candidate-qualification")
            self.assertEqual(receipt["publication_status"], "candidate-only")
            self.assertEqual(receipt["source"], self.source)
            self.assertEqual(receipt["artifact_id"], next(
                artifact["id"] for artifact in self.artifacts if target_id in artifact["name"]))
            self.assertNotIn("signature", receipt)
            self.assertNotIn("manifest_digest", receipt)

    def test_parent_run_is_required_and_signer_child_run_is_not_a_builder_run(self):
        values = list(self.inputs())
        values[0]["workflow"] = catalog.PRODUCER_WORKFLOW
        with self.assertRaises(pipeline.PipelineError):
            self.qualify(values)

    def test_missing_independent_runner_toolchain_evidence_cannot_qualify(self):
        with self.assertRaises(pipeline.PipelineError):
            self.qualify(trusted_environment=None)
        with self.assertRaises(pipeline.PipelineError):
            self.qualify(trusted_environment={})

    def test_only_trusted_workflow_dispatch_context_can_qualify(self):
        for event in ("push", "pull_request", "schedule", "workflow_dispatch-spoof"):
            values = list(self.inputs())
            values[0]["event_name"] = event
            with self.subTest(event=event), self.assertRaises(pipeline.PipelineError):
                self.qualify(values)

    def test_rejects_missing_failed_duplicate_or_unexpected_builder_jobs(self):
        for variant in ("missing", "failed", "duplicate", "extra", "spoofed-output"):
            values = list(self.inputs())
            jobs = values[3]
            if variant == "missing":
                jobs.pop()
            elif variant == "failed":
                jobs[0]["conclusion"] = "failure"
            elif variant == "duplicate":
                jobs.append(copy.deepcopy(jobs[0]))
            elif variant == "extra":
                jobs.append({**jobs[0], "job_id": "build-unreviewed"})
            else:
                jobs[0]["outputs"]["artifact_id"] = "101.0"
            with self.subTest(variant=variant), self.assertRaises(pipeline.PipelineError):
                self.qualify(values)

    def test_rejects_artifact_name_attempt_ancestry_and_identity_substitution(self):
        for variant in ("id", "name", "attempt", "run", "sha", "branch", "expired", "duplicate"):
            values = list(self.inputs())
            artifacts = values[4]
            if variant == "id":
                artifacts[0]["id"] += 1000
            elif variant == "name":
                artifacts[0]["name"] = artifacts[1]["name"]
            elif variant == "attempt":
                artifacts[0]["name"] = artifacts[0]["name"].replace("-1234-2", "-1234-1")
            elif variant in ("run", "sha", "branch"):
                artifacts[0]["workflow_run"][{"run": "id", "sha": "head_sha", "branch": "head_branch"}[variant]] = "stale"
            elif variant == "expired":
                artifacts[0]["expired"] = True
            else:
                artifacts.append(copy.deepcopy(artifacts[0]))
            with self.subTest(variant=variant), self.assertRaises(pipeline.PipelineError):
                self.qualify(values)

    def test_rejects_substituted_candidate_archive_and_stale_policy(self):
        for variant in ("candidate", "archive", "candidate-sha", "archive-sha", "policy", "target", "runner", "catalog"):
            values = list(self.inputs())
            builder, downloads = values[3], values[5]
            item = downloads[101]
            if variant == "archive":
                item["archive"] = downloads[102]["archive"]
            elif variant == "archive-sha":
                builder[0]["outputs"]["archive_sha256"] = "d" * 64
            else:
                document = json.loads(item["candidate"])
                if variant == "candidate":
                    document = json.loads(downloads[102]["candidate"])
                elif variant == "policy":
                    document["policy_identity"] = "d" * 64
                elif variant == "target":
                    document["catalog_target"] = "ios-arm64"
                elif variant == "runner":
                    document["runner"] = "windows-2022"
                elif variant == "catalog":
                    document["catalog_sha256"] = "d" * 64
                item["candidate"] = json.dumps(document, sort_keys=True).encode()
                if variant != "candidate-sha":
                    builder[0]["outputs"]["candidate_sha256"] = sha(item["candidate"])
            with self.subTest(variant=variant), self.assertRaises(pipeline.PipelineError):
                self.qualify(values)

    def test_rejects_malformed_json_and_missing_download_without_verifying_archives(self):
        for variant in ("duplicate-json-key", "missing", "bad-source", "bad-catalog"):
            values = list(self.inputs())
            if variant == "duplicate-json-key":
                values[5][101]["candidate"] = b'{"schema_version":1,"schema_version":1}'
                values[3][0]["outputs"]["candidate_sha256"] = sha(values[5][101]["candidate"])
            elif variant == "missing":
                values[5].pop(101)
            elif variant == "bad-source":
                values[0]["repository"] = "attacker/klogg"
            else:
                values[1]["targets"].pop("ios-arm64")
            with self.subTest(variant=variant), mock.patch.object(pipeline, "verify_core") as verifier:
                with self.assertRaises(pipeline.PipelineError):
                    self.qualify(values)
                verifier.assert_not_called()

    def test_core_verifier_rejects_forged_archive_even_with_matching_builder_hashes(self):
        values = list(self.inputs())
        candidate = json.loads(values[5][101]["candidate"])
        values[5][101]["archive"] = b"not a core"
        values[3][0]["outputs"]["archive_sha256"] = sha(values[5][101]["archive"])
        candidate["archive"]["sha256"] = sha(values[5][101]["archive"])
        candidate["archive"]["size"] = len(values[5][101]["archive"])
        values[5][101]["candidate"] = json.dumps(candidate).encode()
        values[3][0]["outputs"]["candidate_sha256"] = sha(values[5][101]["candidate"])
        with self.assertRaisesRegex(pipeline.PipelineError, "binary-only core"):
            self.qualify(values)

    def test_repo_entrypoint_derives_catalog_and_keys_instead_of_trusting_candidate(self):
        source, _, _, builders, artifacts, downloads, _ = self.inputs()
        receipts = pipeline.qualify_from_repo(ROOT, source, builders, artifacts, downloads,
                                               trusted_environment=self.environment)
        self.assertEqual(set(receipts), set(catalog.TARGETS))
        with mock.patch.object(pipeline, "compute_identities", return_value={
                key: dict(value, core_identity="d" * 64)
                for key, value in self.identities.items()}):
            with self.assertRaises(pipeline.PipelineError):
                pipeline.qualify_from_repo(ROOT, source, builders, artifacts, downloads,
                                           trusted_environment=self.environment)

    def test_oversized_candidate_is_rejected_before_json_parsing(self):
        values = list(self.inputs())
        values[5][101]["candidate"] = b" " * (1024 * 1024 + 1)
        values[3][0]["outputs"]["candidate_sha256"] = sha(values[5][101]["candidate"])
        with mock.patch.object(pipeline, "verify_core") as verifier:
            with self.assertRaisesRegex(pipeline.PipelineError, "oversized candidate JSON"):
                self.qualify(values)
            verifier.assert_not_called()

    def test_trusted_environment_evidence_rejects_spoofed_runner_or_ios_toolchain(self):
        evidence = {target_id: {"runner": row["runner"],
                                **({"toolchain": row["toolchain"]} if "toolchain" in row else {})}
                    for target_id, row in self.catalog["targets"].items()}
        self.assertEqual(set(self.qualify(trusted_environment=evidence)), set(catalog.TARGETS))
        for target_id, field, spoof in (("adb-linux-x86_64", "runner", "windows-2022"),
                                        ("ios-arm64", "toolchain", {"ninja": "0.0"})):
            altered = copy.deepcopy(evidence)
            altered[target_id][field] = spoof
            with self.subTest(target_id=target_id), self.assertRaises(pipeline.PipelineError):
                self.qualify(trusted_environment=altered)


class WindowsNativeQualityEnrollmentTest(unittest.TestCase):
    WORKFLOW = ROOT / ".github/workflows/ci-build.yml"
    TARGET_GUARD = "${{ env.KLOGG_ADB_HELPER_TARGET == 'windows-x86_64' }}"
    SETUP = "Select native Windows Python for dependency contracts"
    TEST = "Run native Windows dependency filesystem contracts"
    JOBS = {"BuildAdbLinuxX64": "linux-x86_64", "BuildAdbLinuxArm64": "linux-arm64",
            "BuildAdbWindowsX64": "windows-x86_64", "BuildAdbMacX64": "macos-x86_64",
            "BuildAdbMacArm64": "macos-arm64"}
    JOB_GUARD = ("${{ ((github.event_name != 'workflow_dispatch' || (inputs.environment-mode == 'off' "
                 "&& inputs.dependency-mode == 'off')) || (github.event_name == 'workflow_dispatch' "
                 "&& (inputs.dependency-mode == 'qualify' || inputs.dependency-mode == 'publish'))) "
                 "&& !contains(github.event.head_commit.message, '[skip ci]') }}")
    UNITTEST = ('unittest.main(module=None, argv=["unittest", "-v", '
                '"test_ci_dependency_windows_security", "test_ci_dependency_core"])')

    def enrollment(self, text):
        rows = [QUALITY.workflow_step_fields(step) for step in
                QUALITY.workflow_job_steps(text)["BuildAdbWindowsX64"]]
        result = []
        for name in (self.SETUP, self.TEST):
            matches = [(index, fields, children) for index, (fields, children) in enumerate(rows)
                       if fields.get("name") == name]
            self.assertEqual(len(matches), 1, "missing mandatory Windows contract path: " + name)
            result.append(matches[0])
        return rows, result

    def assert_enrollment(self, text):
        rows, ((setup_index, setup, setup_children), (test_index, test, test_children)) = self.enrollment(text)
        self.assertTrue(rows[0][0]["uses"].startswith("actions/checkout@"))
        self.assertEqual(rows[0][1]["with"], {"persist-credentials": "false"})
        self.assertEqual((setup_index, test_index), (1, 2))
        self.assertEqual(setup, {"name": self.SETUP, "id": "windows-contract-python",
                                 "if": self.TARGET_GUARD, "with": "",
                                 "uses": "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97"})
        self.assertEqual(setup_children, {"with": {"python-version": "3.14", "architecture": "x64",
                                                  "update-environment": "false"}})
        self.assertEqual(set(test), {"name", "if", "shell", "working-directory", "env", "run"})
        self.assertEqual(test["if"], self.TARGET_GUARD)
        self.assertEqual(test["shell"], "pwsh")
        self.assertEqual(test["working-directory"], "tests/scripts")
        self.assertEqual(test_children, {"env": {
            "KLOGG_NATIVE_PYTHON": "${{ steps.windows-contract-python.outputs.python-path }}"}})
        raw = QUALITY.workflow_job_steps(text)["BuildAdbWindowsX64"][test_index]
        begin = next(index for index, line in enumerate(raw) if line.strip() == "run: |")
        script = textwrap.dedent("\n".join(raw[begin + 1:])).strip()
        self.assertTrue(script.startswith("$source = @'\n"))
        source, invocation = script[len("$source = @'\n"):].split("\n'@\n", 1)
        self.assertEqual(invocation, "& $env:KLOGG_NATIVE_PYTHON -c $source\nexit $LASTEXITCODE")
        body = ast.parse(source).body
        self.assertEqual([type(node) for node in body], [ast.Import, ast.Import, ast.If, ast.Import, ast.Expr])
        self.assertEqual([(node.names[0].name, node.names[0].asname) for node in (body[0], body[1], body[3])],
                         [("os", None), ("sys", None), ("unittest", None)])
        self.assertTrue(all(len(node.names) == 1 for node in (body[0], body[1], body[3])))
        platform = body[2]
        self.assertEqual(ast.dump(platform.test), ast.dump(ast.parse(
            'os.name != "nt" or sys.platform != "win32"', mode="eval").body))
        self.assertEqual(platform.orelse, [])
        self.assertEqual(len(platform.body), 1)
        self.assertIsInstance(platform.body[0], ast.Raise)
        self.assertIsInstance(platform.body[0].exc, ast.Call)
        self.assertEqual(ast.dump(platform.body[0].exc.func), ast.dump(ast.Name(id="SystemExit", ctx=ast.Load())))
        self.assertEqual(ast.dump(body[4].value), ast.dump(ast.parse(self.UNITTEST).body[0].value))
        blocks = QUALITY.workflow_job_blocks(text)
        for job, target in self.JOBS.items():
            self.assertEqual(QUALITY.workflow_job_direct_value(blocks[job], "if"), self.JOB_GUARD)
            environment = QUALITY.workflow_mapping_block(blocks[job], "env", 4)
            self.assertEqual(environment["KLOGG_ADB_HELPER_TARGET"][0], target)
            shared = [QUALITY.workflow_step_fields(step) for step in QUALITY.workflow_job_steps(text)[job]]
            self.assertEqual(shared, rows)
            for event, environment, dependency, active in (
                ("pull_request", "off", "off", True), ("push", "off", "off", True),
                ("workflow_dispatch", "off", "off", True),
                ("workflow_dispatch", "off", "qualify", True),
                ("workflow_dispatch", "off", "publish", True),
                ("workflow_dispatch", "qualify", "off", False),
                ("workflow_dispatch", "publish", "off", False),
                ("workflow_dispatch", "off", "observe-ios-host", False),
            ):
                self.assertEqual(bool(event_expression(self.JOB_GUARD, event, environment, dependency)), active)
                for fields in (setup, test):
                    condition = fields["if"].replace("env.KLOGG_ADB_HELPER_TARGET", repr(target))
                    self.assertEqual(active and bool(event_expression(condition, event, environment, dependency)),
                                     active and target == "windows-x86_64")
        return platform

    def test_real_windows_builder_enrolls_unmocked_contracts_for_ordinary_and_dependency_events(self):
        self.assert_enrollment(self.WORKFLOW.read_text())

    def test_non_native_platforms_fail_before_unittest_import_or_loading(self):
        platform = self.assert_enrollment(self.WORKFLOW.read_text())
        module = ast.Module(body=[platform], type_ignores=[])
        for os_name, sys_platform in (("posix", "darwin"), ("posix", "win32"), ("nt", "msys")):
            for optimize in (0, 2):
                with self.subTest(os_name=os_name, sys_platform=sys_platform, optimize=optimize), self.assertRaisesRegex(
                        SystemExit, "Native Windows CPython required"):
                    exec(compile(module, "<native Windows platform boundary>", "exec", optimize=optimize), {
                        "os": types.SimpleNamespace(name=os_name),
                        "sys": types.SimpleNamespace(platform=sys_platform, executable="unsupported-python")})

    def test_real_enrollment_mutations_cannot_skip_native_proof_or_change_build_path(self):
        text = self.WORKFLOW.read_text()
        block = "\n".join(QUALITY.workflow_job_blocks(text)["BuildAdbLinuxX64"])
        mutations = (
            ('KLOGG_ADB_HELPER_TARGET: linux-x86_64', 'KLOGG_ADB_HELPER_TARGET: windows-x86_64'),
            ('if os.name != "nt" or sys.platform != "win32":\n'
             '              raise SystemExit(f"Native Windows CPython required: {os.name}/{sys.platform} ({sys.executable})")', ''),
            ('if os.name != "nt" or sys.platform != "win32":\n'
             '              raise SystemExit(f"Native Windows CPython required: {os.name}/{sys.platform} ({sys.executable})")',
             'assert os.name == "nt" and sys.platform == "win32"'),
            ('if os.name != "nt" or sys.platform != "win32":', 'if False:'),
            ('if os.name != "nt" or sys.platform != "win32":', 'if os.name != "nt" and sys.platform != "win32":'),
            ('raise SystemExit(f"Native Windows CPython required: {os.name}/{sys.platform} ({sys.executable})")',
             'pass # Native Windows CPython required'),
            ('& $env:KLOGG_NATIVE_PYTHON -c $source', 'python3 -c $source'),
            ('exit $LASTEXITCODE', 'exit 0'),
            ('exit $LASTEXITCODE', 'python3 -c "pass"\n          exit $LASTEXITCODE'),
            ('update-environment: false', 'update-environment: true'),
            ('update-environment: false', '# update-environment: false'),
            ('KLOGG_NATIVE_PYTHON: ${{ steps.windows-contract-python.outputs.python-path }}',
             'PATH: C:/msys64/usr/bin\n          KLOGG_NATIVE_PYTHON: ${{ steps.windows-contract-python.outputs.python-path }}'),
            (self.TARGET_GUARD, "${{ runner.os == 'Windows' }}"),
            ('architecture: x64', 'architecture: x86'),
            ('shell: pwsh\n        working-directory: tests/scripts',
             'shell: pwsh\n        continue-on-error: true\n        working-directory: tests/scripts'),
            (self.TARGET_GUARD, "${{ env.KLOGG_ADB_HELPER_TARGET == 'windows-x86_64' && inputs.dependency-mode == 'qualify' }}"),
            ('"test_ci_dependency_core"', '"test_ci_dependency_pipeline"'),
        )
        for old, new in mutations:
            self.assertIn(old, block)
            mutated = text.replace(block, block.replace(old, new, 1), 1)
            with self.subTest(mutation=new), self.assertRaises(AssertionError):
                self.assert_enrollment(mutated)


if __name__ == "__main__":
    unittest.main()
