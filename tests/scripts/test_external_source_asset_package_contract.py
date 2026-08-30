from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
FIXTURE = ROOT / "tests" / "fixtures" / "external_source_asset_package_contract.json"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
ADB_CMAKE = ROOT / "packaging" / "adb" / "CMakeLists.txt"
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
APPIMAGE_PACKAGE = ROOT / "packaging" / "linux" / "appimage" / "generate_appimage.sh"
MAC_PACKAGE = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
WIN_PREPARE = ROOT / "packaging" / "windows" / "prepare_release.cmd"
WIN_NSIS = ROOT / "packaging" / "windows" / "klogg.nsi"
WIN_PORTABLE = ROOT / "packaging" / "windows" / "7z_klogg_listfile.txt"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
CI_CONTINUOUS = ROOT / ".github" / "workflows" / "ci-continuous.yml"
CI_RELEASE = ROOT / ".github" / "workflows" / "ci-release.yml"
README = ROOT / "README.md"
PUBLICATION_VERIFIER = ROOT / "scripts" / "verify_source_publication_manifest.py"
PUBLICATION_PREPARER = ROOT / "scripts" / "prepare_source_publication.py"


def required_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required external source-asset contract file is missing: {path}")
    return path.read_text(encoding="utf-8")


def required_json(path: pathlib.Path) -> dict:
    document = json.loads(required_text(path))
    if not isinstance(document, dict):
        raise AssertionError(f"expected JSON object in {path}")
    return document


def section(source: str, start: str, end: str) -> str:
    start_index = source.find(start)
    if start_index < 0:
        raise AssertionError(f"missing section start: {start}")
    end_index = source.find(end, start_index + len(start))
    if end_index < 0:
        raise AssertionError(f"missing section end after {start}: {end}")
    return source[start_index:end_index]


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ExternalSourceAssetPackageContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = required_json(FIXTURE)
        cls.adb = cls.contract["source_sets"]["adb-helper"]
        cls.ios = cls.contract["source_sets"]["ios-native"]

    def test_source_publication_identity_accepts_ci_calver_with_build_number(self):
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "from source_publication_identity import published_source_name; "
                    "print(published_source_name('26.08.28.1718', 'adb-helper', 'a' * 64))"
                ),
            ],
            cwd=ROOT / "scripts",
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "klogg-v26.08.28.1718-adb-helper-source-aaaaaaaaaaaa.tar.gz",
        )

    def test_publication_identity_rejects_package_path_escapes_before_copy(self):
        code = """
from source_publication_identity import SourcePublicationIdentityError, validate_asset_file_name
for name in ("../victim", "..\\\\victim", "/tmp/victim"):
    try:
        validate_asset_file_name(name)
    except SourcePublicationIdentityError:
        pass
    else:
        raise SystemExit("accepted " + name)
"""
        result = subprocess.run(
            [sys.executable, "-c", code],
            cwd=ROOT / "scripts",
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        preparer = required_text(PUBLICATION_PREPARER)
        self.assertIn('validate_asset_file_name(record.get("name"))', preparer)

    def test_publication_preparer_rejects_source_archive_path_escape(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            component_root = root / "component"
            output = root / "output"
            component_root.mkdir()
            outside = root / "outside.tar.gz"
            outside.write_bytes(b"outside source\n")
            receipt = {
                "receipt_kind": "component-source-set",
                "component": "adb-helper",
                "archive": {
                    "file_name": "../outside.tar.gz",
                    "sha256": sha256(outside),
                },
                "package_support_assets": [],
            }
            (component_root / "adb-helper-source-set-receipt.json").write_text(
                json.dumps(receipt), encoding="utf-8"
            )
            code = """
import pathlib, sys
from prepare_source_publication import publish_component
publish_component(
    "adb-helper", pathlib.Path(sys.argv[1]),
    "adb-helper-source-set-receipt.json", "26.08.30", "v26.08.30",
    "https://github.com/ZEACENT/klogg", pathlib.Path(sys.argv[2]),
)
"""
            output.mkdir()
            result = subprocess.run(
                [sys.executable, "-c", code, str(component_root), str(output)],
                cwd=ROOT / "scripts",
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertRegex((result.stdout + result.stderr).lower(), r"unsafe|file name")
            self.assertFalse(any(output.iterdir()))

    def test_publication_preparer_rejects_channel_tag_mismatches(self):
        for channel, tag in (("stable", "continuous"), ("continuous", "v26.08.28.1718")):
            with self.subTest(channel=channel, tag=tag):
                result = subprocess.run(
                    [
                        sys.executable,
                        str(PUBLICATION_PREPARER),
                        "--channel",
                        channel,
                        "--evidence-level",
                        "validation",
                        "--tag",
                        tag,
                        "--version",
                        "26.08.28.1718",
                        "--commit",
                        "a" * 40,
                        "--base-url",
                        "https://github.com/ZEACENT/klogg",
                        "--adb-source-root",
                        "missing-adb",
                        "--ios-source-root",
                        "missing-ios",
                        "--output",
                        "unused-output",
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("channel/tag/version mismatch", result.stderr)

    def test_package_staging_uses_explicit_adb_allowlists_without_source_archive(self):
        adb_archive = self.adb["archive_file"]
        retained = [
            asset["file_name"] for asset in self.adb["package_support_assets"]
        ] + [self.adb["receipt_file"]]
        adb_cmake = required_text(ADB_CMAKE)
        app_cmake = required_text(APP_CMAKE)

        combined = "\n".join((adb_cmake, app_cmake))
        for name in retained:
            self.assertIn(name, combined, f"ADB package staging omits {name}")
        for runtime in ("helpers", "adb"):
            self.assertIn(runtime, combined)
        self.assertRegex(
            combined,
            r"copy_directory[^\n]*ADB_HELPER_(?:ARTIFACT_ROOT|STAGE)[^\n]*/helpers",
            "the verified helper directory must retain its target-specific runtime closure",
        )
        self.assertNotRegex(
            adb_cmake,
            r"install\(DIRECTORY\s+\"\$\{_klogg_adb_release_stage\}/\"",
            "Linux installers must not install the complete release/source directory",
        )
        self.assertNotRegex(
            app_cmake,
            r"copy_directory\s+\"\$\{KLOGG_ADB_HELPER_RELEASE_STAGE\}\"",
            "macOS bundles must not copy the complete ADB release/source directory",
        )
        self.assertRegex(
            app_cmake,
            r"rm -rf[^\n]*SharedSupport/adb-helper",
            "incremental package staging must remove stale release-only assets first",
        )
        staged_lines = "\n".join(
            line
            for line in combined.splitlines()
            if "copy" in line.lower() or "install(" in line.lower()
        )
        self.assertNotIn(adb_archive, staged_lines)

        windows_files = {
            "prepare": required_text(WIN_PREPARE),
            "installer": required_text(WIN_NSIS),
            "portable": required_text(WIN_PORTABLE),
        }
        for label, source in windows_files.items():
            with self.subTest(windows=label):
                for name in retained:
                    self.assertIn(name, source, f"{label} package omits {name}")
                self.assertNotIn(adb_archive, source)
        self.assertNotRegex(
            windows_files["prepare"],
            re.compile(r"adb-helper-release\\\*", re.IGNORECASE),
        )
        self.assertRegex(
            windows_files["prepare"],
            re.compile(
                r"if exist .*release\\adb-helper-assets .*rmdir /s /q .*release\\adb-helper-assets",
                re.IGNORECASE,
            ),
            "incremental Windows staging must remove stale source-only assets first",
        )
        self.assertNotRegex(
            windows_files["installer"],
            re.compile(r"File\s+/r\s+release\\adb-helper-assets\\\*", re.IGNORECASE),
        )
        self.assertNotRegex(
            windows_files["portable"],
            re.compile(r"adb-helper-assets\\\*", re.IGNORECASE),
        )
        for runtime in ("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll", "libusb-1.0.dll"):
            self.assertIn(runtime, windows_files["installer"])
            self.assertIn(runtime, windows_files["portable"])

    def test_linux_packages_install_only_the_klogg_runtime_component(self):
        root_cmake = required_text(ROOT_CMAKE)
        app_cmake = required_text(APP_CMAKE)
        adb_cmake = required_text(ADB_CMAKE)
        appimage = required_text(APPIMAGE_PACKAGE)

        self.assertIn("set(KLOGG_RUNTIME_INSTALL_COMPONENT klogg-runtime)", root_cmake)
        self.assertIn(
            'set(CPACK_COMPONENTS_ALL "${KLOGG_RUNTIME_INSTALL_COMPONENT}")',
            root_cmake,
        )
        self.assertIn(
            'set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};'
            '${KLOGG_RUNTIME_INSTALL_COMPONENT};/")',
            root_cmake,
        )
        self.assertGreaterEqual(
            "\n".join((root_cmake, app_cmake, adb_cmake)).count(
                "COMPONENT ${KLOGG_RUNTIME_INSTALL_COMPONENT}"
            ),
            9,
            "every first-party runtime and support install rule must share one component",
        )
        self.assertIn(
            "cmake --install . --component klogg-runtime",
            appimage,
        )
        self.assertNotIn("ninja install", appimage)

    def test_packageable_ci_build_disables_croaring_test_install_graph(self):
        third_party = required_text(THIRD_PARTY_CMAKE)
        croaring = section(
            third_party,
            "cpmaddpackage(\n  NAME CRoaring",
            "\n\n# Patched dependencies",
        )
        self.assertIn(
            '"ENABLE_ROARING_TESTS OFF"',
            croaring,
            "ci_build must not leave CMocka test install rules for CPack",
        )

    def test_macos_stages_ios_runtime_and_support_receipts_but_not_source_tarball(self):
        action = required_text(MAC_PACKAGE)
        staging = section(
            action,
            "- name: Verify staged iOS native bundle",
            "- name: Verify source-built ADB helper before DMG",
        )
        retained = [
            asset["file_name"] for asset in self.ios["package_support_assets"]
        ] + [
            self.ios["receipt_file"],
            "ios-native-build-receipt.json",
        ]
        for name in retained:
            self.assertIn(name, staging, f"macOS package staging omits {name}")
        self.assertIn("*.dylib", staging, "macOS package must retain the native runtime closure")
        self.assertNotIn(
            self.ios["archive_file"],
            staging,
            "release-only iOS source tarball must be absent before application signing",
        )

    def test_macos_qualification_reverifies_ios_closure_inside_final_dmg(self):
        action = required_text(MAC_PACKAGE)
        qualification = section(
            action,
            "- name: Sign and verify macOS disk image",
            "- name: Mac symbols",
        )
        for marker in (
            "hdiutil attach",
            "mounted_app",
            "verify_ios_native_stack.py",
            "--verify-package-receipt",
            "mounted_helper",
            "smoke_adb_helper.py",
            "verify_adb_helper_artifact.py",
            '--package-root "$mount_dir"',
            "Contents/Frameworks/ios-native",
            "Contents/SharedSupport/ios-native",
        ):
            self.assertIn(marker, qualification)

    def test_stable_release_publishes_content_addressed_adb_and_ios_sources_with_manifest(self):
        workflow = required_text(CI_RELEASE)
        preparer = required_text(PUBLICATION_PREPARER)
        manifest = self.contract["publication_manifest"]["file_name"]
        for name in (
            self.adb["receipt_file"],
            self.ios["receipt_file"],
            manifest,
            "adb-helper",
            "ios-native",
        ):
            self.assertIn(name, workflow)
        self.assertIn(
            "published_source_name(version, component, archive_hash)",
            preparer,
        )
        for component in ("adb-helper", "ios-native"):
            self.assertIn(f'"{component}"', preparer)
        self.assertIn("prepare_source_publication.py", workflow)
        self.assertIn("verify_source_publication_manifest.py", workflow)
        self.assertRegex(workflow, r"(?i)channel[^\n]*stable")
        self.assertIn('"mutable": args.channel == "continuous"', preparer)
        upload = workflow.index("- name: Create GitHub Release")
        post_upload = workflow.find("verify_source_publication_manifest.py", upload)
        self.assertGreater(post_upload, upload, "stable publication must verify after upload")
        self.assertRegex(workflow[upload:], r"gh\s+(?:api|release\s+download)")

    def test_stable_release_defaults_to_validation_and_keeps_signed_evidence_strict(self):
        build = required_text(CI_BUILD)
        release = required_text(CI_RELEASE)
        self.assertIn("qualification-mode:", build)
        self.assertIn("- release", build)
        self.assertIn("evidence-level:", release)
        evidence_input = release.split("evidence-level:", 1)[1].split("jobs:", 1)[0]
        self.assertIn("default: validation", evidence_input)
        self.assertIn("- validation", evidence_input)
        self.assertIn("- signed", evidence_input)
        self.assertIn("--evidence-level \"${KLOGG_EVIDENCE_LEVEL}\"", release)
        self.assertRegex(
            release,
            r"KLOGG_EVIDENCE_LEVEL.*signed[\s\S]+adb-helper-signing-receipt\.json[\s\S]+adb-helper-notarization-receipt\.json",
        )
        for secret in (
            "CODESIGN_BASE64",
            "CODESIGN_PASSWORD",
            "APPLE_DEVELOPER_ID_APPLICATION",
            "NOTARIZATION_USERNAME",
            "NOTARIZATION_TEAM",
            "NOTARIZATION_PASSWORD",
        ):
            self.assertNotIn(f"secrets.{secret}", release)

    def test_required_ci_does_not_publish_and_workflow_run_publisher_is_trusted(self):
        build = required_text(CI_BUILD)
        for marker in (
            "CreatePreRelease:",
            "Create continuous candidate draft",
            "tag_name=continuous",
            "softprops/action-gh-release",
        ):
            self.assertNotIn(marker, build)

        continuous = required_text(CI_CONTINUOUS)
        self.assertIn("workflow_run:", continuous)
        self.assertRegex(continuous, r"workflows:\s*\[\s*[\"']CI Build[\"']\s*\]")
        self.assertIn("types: [completed]", continuous)
        trusted = (
            "github.event.workflow_run.conclusion == 'success'"
            " && github.event.workflow_run.event == 'push'"
            " && github.event.workflow_run.head_branch == 'master'"
            " && github.event.workflow_run.head_repository.full_name == github.repository"
        )
        self.assertIn(trusted, " ".join(continuous.split()))
        self.assertIn("actions: read", continuous)
        self.assertIn("contents: write", continuous)
        self.assertIn("cancel-in-progress: false", continuous)
        self.assertIn("github.token", continuous)
        self.assertNotRegex(continuous, r"secrets\.(?:CODESIGN|APPLE|NOTARIZATION)")
        readme = required_text(README)
        self.assertIn("continuous release", readme.lower())
        self.assertRegex(readme, r"(?i)macOS[^\n]*unsigned")

    def test_source_asset_producers_bind_release_version_and_flat_checksums(self):
        build = required_text(CI_BUILD)
        stable = required_text(CI_RELEASE)
        adb_producer = section(build, "  PrefetchAdbHelperSources:", "  BuildAdbHelpers:")
        ios_producer = section(build, "  BuildIosNativeStacks:", "  MacPackages:")
        for producer in (adb_producer, ios_producer):
            self.assertRegex(producer, r"needs:\s*\[[^\]]*SaveVersion")
            self.assertIn("--version", producer)
            self.assertIn("--base-url", producer)
        self.assertNotIn("sha256sum --binary ./packages-publication/*", stable)
        self.assertIn("./*adb-helper* ./ADB-HELPER-*", stable)
        self.assertRegex(
            stable,
            r"\(cd ./packages-publication && sha256sum --binary \./\* > "
            r"\.\./packages-bin/source-publication-sha256\.txt\)",
        )

    def test_publication_preparer_requires_adb_and_explicit_ios_source_qualifications(self):
        preparer = required_text(PUBLICATION_PREPARER)
        for marker in (
            "source_set_receipt_sha256",
            "--ios-qualification-receipt",
            "ios-native-package-verification",
            "missing external iOS qualification for DMG",
        ):
            self.assertIn(marker, preparer)
        stable = required_text(CI_RELEASE)
        self.assertIn("ios-native-dmg-package-verification.json", stable)
        self.assertIn("--ios-qualification-receipt", stable)
        self.assertNotIn("--ios-qualification-receipt", required_text(CI_BUILD))

    def test_stable_release_selects_exact_trusted_ci_artifacts_without_expression_injection(self):
        workflow = required_text(CI_RELEASE)
        self.assertIn('test "${GITHUB_REF}" = "refs/heads/master"', workflow)
        self.assertIn("KLOGG_REQUESTED_CI_RUN_ID: ${{ github.event.inputs.ci-run-id }}", workflow)
        selection = section(
            workflow,
            "- name: Select trusted CI run",
            "- name: Download artifacts for selected CI workflow",
        )
        self.assertNotIn("${{ github.event.inputs.ci-run-id }}", selection)
        self.assertRegex(selection, r"\^\[0-9\]\+\$")
        for marker in (
            'run_path="$(gh api',
            'run_repository="$(gh api',
            'run_branch="$(gh api',
            'run_event="$(gh api',
            'run_conclusion="$(gh api',
            'KLOGG_CI_RUN_ID=',
            'KLOGG_CI_RUN_SHA=',
            '".github/workflows/ci-build.yml"',
            '"${GITHUB_REPOSITORY}"',
            '"master"',
            '"success"',
            "event=push",
            'KLOGG_EVIDENCE_LEVEL" = validation',
            "workflow-run API does not expose workflow_dispatch inputs",
            "/branches/master",
        ):
            self.assertIn(marker, selection)
        self.assertNotIn('.inputs["qualification-mode"]', selection)
        self.assertIn("run_id: ${{ env.KLOGG_CI_RUN_ID }}", workflow)

        metadata = workflow.index("klogg_commit.txt")
        checkout = workflow.find('git checkout --detach "${KLOGG_SOURCE_COMMIT}"', metadata)
        extract = workflow.index("- name: Extract package tarballs")
        self.assertGreater(checkout, metadata)
        self.assertLess(checkout, extract)
        initialization = workflow[metadata:extract]
        self.assertIn('test "${KLOGG_SOURCE_COMMIT}" = "${KLOGG_CI_RUN_SHA}"', initialization)
        self.assertIn("git merge-base --is-ancestor", initialization)
        changelog = section(workflow, "- name: Generate Changelog", "- name: Display structure")
        self.assertIn('--to-ref "${KLOGG_SOURCE_COMMIT}"', changelog)

    def test_stable_retry_recreates_a_clean_draft_asset_set(self):
        stable = required_text(CI_RELEASE)
        stable_cleanup = stable.index("- name: Clean stale stable draft")
        stable_create = stable.index("- name: Create GitHub Release")
        self.assertLess(stable_cleanup, stable_create)
        cleanup = stable[stable_cleanup:stable_create]
        self.assertIn("draft", cleanup)
        self.assertIn("gh api -X DELETE", cleanup)
        self.assertIn('candidate_tag="${final_tag}-candidate"', cleanup)
        self.assertIn("/git/ref/tags/${tag}", cleanup)
        self.assertNotIn(
            'git/refs/tags/${tag}" >/dev/null 2>&1 || true',
            cleanup,
            "stale stable tag deletion must fail closed",
        )
        self.assertNotIn("Create continuous candidate draft", required_text(CI_BUILD))

    def test_stable_release_uses_selected_ci_commit_and_remains_draft_until_verified(self):
        workflow = required_text(CI_RELEASE)
        self.assertIn("klogg_commit.txt", workflow)
        self.assertIn("KLOGG_SOURCE_COMMIT", workflow)
        publication = section(
            workflow, "- name: Prepare coherent stable package and source publication", "- name: Discord notification"
        )
        self.assertNotIn('--commit "${GITHUB_SHA}"', publication)
        self.assertIn("target_commitish: ${{ env.KLOGG_SOURCE_COMMIT }}", publication)
        create = publication.index("- name: Create GitHub Release")
        verify = publication.index("- name: Verify stable source publication after upload")
        self.assertLess(create, verify)
        self.assertIn("draft: true", publication[create:verify])
        self.assertIn("tag_name: v${{ env.KLOGG_VERSION }}-candidate", publication[create:verify])
        verification = publication[verify:]
        draft_verification = verification[: verification.index("- name: Recheck master tip")]
        self.assertIn(".target_commitish", draft_verification)
        self.assertIn(
            'test "${candidate_target}" = "${KLOGG_SOURCE_COMMIT}"',
            draft_verification,
        )
        self.assertNotIn("/git/ref/tags/", draft_verification)
        promotion = publication.find("draft=false", verify)
        self.assertGreater(promotion, verify, "stable release must publish only after verification")

    def test_release_publication_is_explicit_and_verifies_before_promotion(self):
        self.assertNotIn("Create continuous candidate draft", required_text(CI_BUILD))
        release = required_text(CI_RELEASE)
        create = release.index("- name: Create GitHub Release")
        verify = release.index("- name: Verify stable source publication after upload")
        stale = release.index("- name: Recheck master tip before stable promotion")
        promote = release.index("- name: Publish verified stable release")
        self.assertLess(create, verify)
        self.assertLess(verify, stale)
        self.assertLess(stale, promote)
        self.assertIn("draft: true", release[create:verify])
        promotion = release[promote:]
        self.assertIn("draft=false", promotion)
        self.assertGreaterEqual(promotion.count("/branches/master"), 2)
        self.assertIn("Roll back failed stable promotion", promotion)
        self.assertIn('releases/${RELEASE_ID}', promotion)
        self.assertIn('delete_ref_if_present "$final_tag"', promotion)
        before_create = release.index("- name: Recheck master tip before stable draft creation")
        self.assertLess(before_create, create)
        for position in (before_create, stale):
            check = release[position : release.find("\n    - name:", position + 1)]
            self.assertIn("/branches/master", check)
            self.assertIn("KLOGG_SOURCE_COMMIT", check)

    def test_continuous_publication_is_transactional_sha_bound_and_rollback_safe(self):
        workflow = required_text(CI_CONTINUOUS)
        ordered = [
            "Create continuous candidate draft",
            "Verify continuous candidate after upload",
            "Recheck master tip before continuous promotion",
            "Park existing continuous release for rollback",
            "Promote verified continuous candidate",
            "Reverify promoted continuous release",
            "Roll back failed continuous promotion",
        ]
        positions = [workflow.index(marker) for marker in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("draft: true", workflow[positions[0] : positions[1]])
        rollback = workflow[positions[-1] :]
        self.assertIn("failure() || cancelled()", rollback)
        recovery = workflow.index("Recover interrupted continuous transaction")
        self.assertLess(recovery, positions[0])
        reconciliation = workflow[recovery:positions[0]]
        self.assertIn("continuous-rollback-", reconciliation)
        self.assertIn("releases/tags/continuous", reconciliation)
        self.assertIn("Restore parked continuous release", reconciliation)
        self.assertIn("Remove stale parked continuous release", reconciliation)
        self.assertIn("verify_source_publication_manifest.py", reconciliation)
        self.assertIn("live continuous release is coherent", reconciliation)
        self.assertIn('candidate_tag="continuous-candidate-${KLOGG_CI_RUN_ID}"', workflow)
        self.assertIn('backup_tag="continuous-rollback-${KLOGG_CI_RUN_ID}"', workflow)
        self.assertNotIn("continuous-candidate-${GITHUB_RUN_ID}", workflow)
        self.assertNotIn("continuous-rollback-${GITHUB_RUN_ID}", workflow)
        self.assertIn("run-id: ${{ github.event.workflow_run.id }}", workflow)
        self.assertIn("github-token: ${{ github.token }}", workflow)
        self.assertIn("github.event.workflow_run.head_sha", workflow)
        self.assertIn('test "${KLOGG_SOURCE_COMMIT}" = "${KLOGG_CI_RUN_SHA}"', workflow)
        reverify = section(
            workflow,
            "- name: Reverify promoted continuous release",
            "- name: Roll back failed continuous promotion",
        )
        self.assertIn("/branches/master", reverify)
        self.assertIn("KLOGG_SOURCE_COMMIT", reverify)
        promotion = section(
            workflow,
            "- name: Promote verified continuous candidate",
            "- name: Reverify promoted continuous release",
        )
        self.assertIn('candidate_tag="continuous-candidate-${KLOGG_CI_RUN_ID}"', promotion)
        self.assertNotIn("2>&1 || true", promotion)
        for marker in (
            "continuous-candidate-",
            "continuous-rollback-",
            "tag_name=continuous",
            "expected-channel continuous",
            "expected-evidence-level validation",
        ):
            self.assertIn(marker, workflow)


class SourcePublicationManifestVerifierContractTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def write_asset(self, name: str, content: bytes) -> pathlib.Path:
        path = self.root / name
        path.write_bytes(content)
        return path

    def make_manifest(self, evidence_level: str = "signed") -> tuple[pathlib.Path, dict]:
        version = "26.08.27"
        tag = f"v{version}"
        commit = "a" * 40
        components = {}
        source_set_hashes = {}
        for component in ("adb-helper", "ios-native"):
            archive_content = f"{component} immutable source closure\n".encode()
            archive_hash = hashlib.sha256(archive_content).hexdigest()
            archive_name = f"klogg-v{version}-{component}-source-{archive_hash[:12]}.tar.gz"
            archive = self.write_asset(archive_name, archive_content)
            self.write_asset(
                f"{archive_name}.sha256",
                f"{archive_hash}  {archive_name}\n".encode(),
            )
            receipt_name = f"{component}-source-set-receipt.json"
            support = self.write_asset(
                f"{component}-source-offer.txt", f"{component} source offer\n".encode()
            )
            self.write_asset(
                f"{support.name}.sha256",
                f"{sha256(support)}  {support.name}\n".encode(),
            )
            receipt_document = {
                "schema_version": 1,
                "receipt_kind": "component-source-set",
                "component": component,
                "lock_sha256": "1" * 64,
                "archive": {
                    "file_name": f"{component}-corresponding-source.tar.gz",
                    "sha256": archive_hash,
                },
                "source_identity": {
                    "manifest_or_closure_sha256": "2" * 64,
                    "tree_hash_algorithm": "sha256",
                    "final_tree_sha256": "3" * 64,
                },
                "patch_chain_sha256": "4" * 64,
                "package_support_assets": [
                    {
                        "kind": "source-offer",
                        "file_name": support.name,
                        "sha256": sha256(support),
                    }
                ],
                "distribution": {"package_required": False, "release_required": True},
            }
            receipt = self.write_asset(
                receipt_name,
                (json.dumps(receipt_document, sort_keys=True) + "\n").encode(),
            )
            receipt_hash = sha256(receipt)
            self.write_asset(
                f"{receipt_name}.sha256",
                f"{receipt_hash}  {receipt_name}\n".encode(),
            )
            source_set_hashes[component] = receipt_hash
            components[component] = {
                "source_set_receipt": {"file_name": receipt.name, "sha256": receipt_hash},
                "source_archive": {
                    "file_name": archive.name,
                    "source_set_file_name": receipt_document["archive"]["file_name"],
                    "sha256": archive_hash,
                    "url": f"https://github.com/ZEACENT/klogg/releases/download/{tag}/{archive.name}",
                },
            }

        package = self.write_asset("klogg-26.08.27-mac-arm64.dmg", b"qualified package\n")
        package_hash = sha256(package)
        qualification = self.write_asset(
            "adb-helper-dmg-package-verification.json",
            (
                json.dumps(
                    {
                        "receipt_kind": "package-verification",
                        "target": "macos-arm64",
                        "release_qualified": evidence_level == "signed",
                        "source_set_receipt_sha256": source_set_hashes["adb-helper"],
                        "source_helper_sha256": "6" * 64,
                        "helper_sha256": "7" * 64,
                        "required_receipts": [
                            "binary-build",
                            "binary-smoke",
                            "package-verification",
                            "signing",
                            "notarization",
                        ],
                        "verified_receipts": [
                            "binary-build",
                            "binary-smoke",
                            "package-verification",
                            *(["signing", "notarization"] if evidence_level == "signed" else []),
                        ],
                        "packages": [{"name": package.name, "sha256": package_hash}],
                    },
                    sort_keys=True,
                )
                + "\n"
            ).encode(),
        )
        ios_package_receipt = self.write_asset(
            f"{package.name}.ios-package.json",
            (
                json.dumps(
                    {
                        "schema_version": 2,
                        "receipt_kind": "ios-native-package",
                        "status": "passed",
                        "source_set_receipt_sha256": source_set_hashes["ios-native"],
                        "architecture": "arm64",
                        "dylibs": [],
                    },
                    sort_keys=True,
                )
                + "\n"
            ).encode(),
        )
        ios_qualification = self.write_asset(
            "ios-native-dmg-package-verification.json",
            (
                json.dumps(
                    {
                        "receipt_kind": "ios-native-package-verification",
                        "qualification": evidence_level,
                        "release_qualified": evidence_level == "signed",
                        "source_set_receipt_sha256": source_set_hashes["ios-native"],
                        "ios_native_package_receipt_sha256": sha256(ios_package_receipt),
                        "packages": [{"name": package.name, "sha256": package_hash}],
                    },
                    sort_keys=True,
                )
                + "\n"
            ).encode(),
        )
        signed_evidence = {}
        if evidence_level == "signed":
            for kind in ("signing", "notarization"):
                evidence = self.write_asset(
                    f"adb-helper-{kind}-receipt.json",
                    (
                        json.dumps(
                            {
                                "receipt_kind": kind,
                                "status": "passed",
                                "target": "macos-arm64",
                                **(
                                    {"identity": "Developer ID Application: Klogg Test"}
                                    if kind == "signing"
                                    else {
                                        "team_id": "TESTTEAM123",
                                        "submission_id": "00000000-0000-0000-0000-000000000001",
                                    }
                                ),
                                "package_sha256": package_hash,
                                "source_helper_sha256": "6" * 64,
                                "signed_helper_sha256": "7" * 64,
                            },
                            sort_keys=True,
                        )
                        + "\n"
                    ).encode(),
                )
                signed_evidence[f"{kind}_receipt"] = {
                    "file_name": evidence.name,
                    "sha256": sha256(evidence),
                }

        linux_package = self.write_asset("klogg-26.08.27-linux.deb", b"linux package\n")
        linux_hash = sha256(linux_package)
        linux_qualification = self.write_asset(
            "adb-helper-deb-package-verification.json",
            (
                json.dumps(
                    {
                        "receipt_kind": "package-verification",
                        "release_qualified": True,
                        "source_set_receipt_sha256": source_set_hashes["adb-helper"],
                        "required_receipts": [
                            "binary-build",
                            "binary-smoke",
                            "package-verification",
                        ],
                        "verified_receipts": [
                            "binary-build",
                            "binary-smoke",
                            "package-verification",
                        ],
                        "packages": [{"name": linux_package.name, "sha256": linux_hash}],
                    },
                    sort_keys=True,
                )
                + "\n"
            ).encode(),
        )
        document = {
            "schema_version": 1,
            "manifest_kind": "klogg-source-publication",
            "channel": "stable",
            "evidence_level": evidence_level,
            "release": {
                "tag": tag,
                "version": version,
                "commit": commit,
                "mutable": False,
            },
            "components": components,
            "packages": [
                {
                    "file_name": package.name,
                    "sha256": package_hash,
                    "qualification_receipt": {
                        "file_name": qualification.name,
                        "sha256": sha256(qualification),
                    },
                    "ios_qualification_receipt": {
                        "file_name": ios_qualification.name,
                        "sha256": sha256(ios_qualification),
                    },
                    "ios_package_receipt": {
                        "file_name": ios_package_receipt.name,
                        "sha256": sha256(ios_package_receipt),
                    },
                    **signed_evidence,
                    "source_sets": source_set_hashes,
                },
                {
                    "file_name": linux_package.name,
                    "sha256": linux_hash,
                    "qualification_receipt": {
                        "file_name": linux_qualification.name,
                        "sha256": sha256(linux_qualification),
                    },
                    "source_sets": {"adb-helper": source_set_hashes["adb-helper"]},
                },
            ],
        }
        manifest = self.root / "klogg-source-publication-manifest.json"
        manifest.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return manifest, document

    def run_verifier(self, manifest: pathlib.Path, evidence_level: str = "signed"):
        self.assertTrue(
            PUBLICATION_VERIFIER.is_file(),
            f"missing publication manifest verifier: {PUBLICATION_VERIFIER}",
        )
        return subprocess.run(
            [
                sys.executable,
                str(PUBLICATION_VERIFIER),
                "--manifest",
                str(manifest),
                "--assets-root",
                str(self.root),
                "--expected-channel",
                "stable",
                "--expected-evidence-level",
                evidence_level,
                "--expected-tag",
                "v26.08.27",
                "--expected-version",
                "26.08.27",
                "--expected-commit",
                "a" * 40,
                "--expected-base-url",
                "https://github.com/ZEACENT/klogg",
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_manifest_verifier_accepts_complete_content_addressed_publication(self):
        manifest, _ = self.make_manifest()
        result = self.run_verifier(manifest)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_validation_evidence_accepts_unsigned_macos_receipts_with_complete_bindings(self):
        manifest, document = self.make_manifest("validation")
        self.assertEqual(document["channel"], "stable")
        self.assertEqual(document["evidence_level"], "validation")
        result = self.run_verifier(manifest, "validation")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_signed_evidence_rejects_missing_or_failed_signing_and_notarization(self):
        for mismatch in ("missing-signing", "failed-notarization"):
            with self.subTest(mismatch=mismatch):
                manifest, document = self.make_manifest("signed")
                if mismatch == "missing-signing":
                    document["packages"][0].pop("signing_receipt")
                else:
                    path = self.root / "adb-helper-notarization-receipt.json"
                    receipt = json.loads(path.read_text(encoding="utf-8"))
                    receipt["status"] = "failed"
                    path.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
                    document["packages"][0]["notarization_receipt"]["sha256"] = sha256(path)
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest, "signed")
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex((result.stdout + result.stderr).lower(), r"sign|notari")

    def test_manifest_verifier_rejects_hash_tag_package_and_source_mismatches(self):
        mutations = {
            "hash": lambda document: document["components"]["adb-helper"][
                "source_archive"
            ].update(sha256="0" * 64),
            "tag": lambda document: document["release"].update(tag="v99.99.99"),
            "package": lambda document: document["packages"][0].update(sha256="0" * 64),
            "source": lambda document: document["packages"][0]["source_sets"].update(
                {"ios-native": "0" * 64}
            ),
        }
        for mismatch, mutate in mutations.items():
            with self.subTest(mismatch=mismatch):
                manifest, valid = self.make_manifest()
                document = copy.deepcopy(valid)
                mutate(document)
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(mismatch, (result.stdout + result.stderr).lower())

    def test_manifest_verifier_rejects_wrong_repository_and_source_set_archive_mapping(self):
        scenarios = {
            "repository": lambda document: document["components"]["adb-helper"][
                "source_archive"
            ].update(
                url=document["components"]["adb-helper"]["source_archive"]["url"].replace(
                    "github.com/ZEACENT/klogg", "example.invalid/wrong/repository"
                )
            ),
            "mapping": lambda document: document["components"]["adb-helper"][
                "source_archive"
            ].update(source_set_file_name="wrong-internal-name.tar.gz"),
        }
        for mismatch, mutate in scenarios.items():
            with self.subTest(mismatch=mismatch):
                manifest, valid = self.make_manifest()
                document = copy.deepcopy(valid)
                mutate(document)
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(), r"url|repository|source|archive"
                )

    def test_manifest_verifier_rejects_tampered_support_sidecar_and_extra_package(self):
        scenarios = ("support", "support-sidecar", "sidecar", "extra-package")
        for mismatch in scenarios:
            with self.subTest(mismatch=mismatch):
                manifest, document = self.make_manifest()
                if mismatch == "support":
                    (self.root / "adb-helper-source-offer.txt").write_text(
                        "tampered source offer\n", encoding="utf-8"
                    )
                elif mismatch == "support-sidecar":
                    (self.root / "adb-helper-source-offer.txt.sha256").unlink()
                elif mismatch == "sidecar":
                    archive_name = document["components"]["adb-helper"]["source_archive"][
                        "file_name"
                    ]
                    (self.root / f"{archive_name}.sha256").write_text(
                        f"{'0' * 64}  {archive_name}\n", encoding="utf-8"
                    )
                else:
                    self.write_asset(
                        "klogg-26.08.27-unqualified-extra-portable.zip",
                        b"not qualified\n",
                    )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(),
                    r"support|checksum|sidecar|unreferenced|package",
                )

    def test_manifest_verifier_rejects_invalid_or_package_mismatched_qualification_receipt(self):
        scenarios = {
            "kind": lambda receipt: receipt.update(receipt_kind="signing"),
            "qualified": lambda receipt: receipt.update(release_qualified=False),
            "filename": lambda receipt: receipt["packages"][0].update(name="other.dmg"),
            "package-hash": lambda receipt: receipt["packages"][0].update(sha256="0" * 64),
        }
        for mismatch, mutate in scenarios.items():
            with self.subTest(mismatch=mismatch):
                manifest, _ = self.make_manifest()
                qualification = self.root / "adb-helper-dmg-package-verification.json"
                receipt = json.loads(qualification.read_text(encoding="utf-8"))
                mutate(receipt)
                qualification.write_text(
                    json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8"
                )
                document = json.loads(manifest.read_text(encoding="utf-8"))
                document["packages"][0]["qualification_receipt"]["sha256"] = sha256(
                    qualification
                )
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(), r"qualification|package"
                )

    def test_manifest_verifier_rejects_unbound_ios_package_receipt_hash(self):
        manifest, document = self.make_manifest("signed")
        qualification = self.root / "ios-native-dmg-package-verification.json"
        receipt = json.loads(qualification.read_text(encoding="utf-8"))
        receipt["ios_native_package_receipt_sha256"] = "5" * 64
        qualification.write_text(
            json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8"
        )
        document["packages"][0]["ios_qualification_receipt"]["sha256"] = sha256(
            qualification
        )
        manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

        result = self.run_verifier(manifest, "signed")

        self.assertNotEqual(result.returncode, 0)
        self.assertRegex((result.stdout + result.stderr).lower(), r"ios|package receipt")

    def test_manifest_verifier_requires_component_qualification_source_set_bindings(self):
        scenarios = ("adb-binding", "ios-missing", "ios-binding", "ios-package-receipt")
        for mismatch in scenarios:
            with self.subTest(mismatch=mismatch):
                manifest, _ = self.make_manifest()
                document = json.loads(manifest.read_text(encoding="utf-8"))
                if mismatch == "adb-binding":
                    path = self.root / "adb-helper-dmg-package-verification.json"
                    receipt = json.loads(path.read_text(encoding="utf-8"))
                    receipt["source_set_receipt_sha256"] = "0" * 64
                    path.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
                    document["packages"][0]["qualification_receipt"]["sha256"] = sha256(path)
                elif mismatch == "ios-missing":
                    document["packages"][0].pop("ios_qualification_receipt")
                else:
                    path = self.root / "ios-native-dmg-package-verification.json"
                    receipt = json.loads(path.read_text(encoding="utf-8"))
                    if mismatch == "ios-binding":
                        receipt["source_set_receipt_sha256"] = "0" * 64
                    else:
                        receipt.pop("ios_native_package_receipt_sha256")
                    path.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
                    document["packages"][0]["ios_qualification_receipt"]["sha256"] = sha256(path)
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex((result.stdout + result.stderr).lower(), r"source|ios")

    def test_manifest_verifier_rejects_unreferenced_source_and_qualification_assets(self):
        extras = (
            "extra-source-set-receipt.json",
            "klogg-v26.08.27-extra-source-000000000000.tar.gz",
            "extra.qualification.json",
            "extra.ios-qualification.json",
            "other-source-publication-manifest.json",
        )
        for extra in extras:
            with self.subTest(extra=extra):
                manifest, _ = self.make_manifest()
                self.write_asset(extra, b"unreferenced\n")
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(), r"unreferenced|publication"
                )

    def test_manifest_verifier_requires_exact_platform_source_coverage(self):
        scenarios = {
            "mac-missing-ios": lambda document: document["packages"][0].update(
                source_sets={"adb-helper": document["packages"][0]["source_sets"]["adb-helper"]}
            ),
            "linux-extra-ios": lambda document: document["packages"][1]["source_sets"].update(
                {"ios-native": document["packages"][0]["source_sets"]["ios-native"]}
            ),
        }
        for mismatch, mutate in scenarios.items():
            with self.subTest(mismatch=mismatch):
                manifest, valid = self.make_manifest()
                document = copy.deepcopy(valid)
                mutate(document)
                manifest.write_text(
                    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("source", (result.stdout + result.stderr).lower())


if __name__ == "__main__":
    unittest.main()
