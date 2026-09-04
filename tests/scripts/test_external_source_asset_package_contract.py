from __future__ import annotations

import copy
import hashlib
import io
import json
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
import urllib.parse


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
RELEASE_DOWNLOAD_RENDERER = ROOT / "scripts" / "render_release_downloads.py"


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
        self.assertNotIn("packages-bin", stable)
        self.assertNotIn("source-publication-sha256.txt", stable)
        self.assertIn("packages-publication/SHA256SUMS", stable)

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
            "Publish verified continuous release transaction",
            "Reverify promoted continuous release",
            "Roll back failed continuous promotion",
        ]
        positions = [workflow.find(marker) for marker in ordered]
        self.assertNotIn(
            -1,
            positions,
            "continuous publication must use one rollback-protected mutation step",
        )
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
        critical = section(
            workflow,
            "- name: Publish verified continuous release transaction",
            "- name: Reverify promoted continuous release",
        )
        for marker in (
            "GITHUB_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}",
            "rollback_continuous_promotion()",
            "trap rollback_continuous_promotion ERR",
            '-f tag_name="$backup_tag" -F draft=true',
            'gh api -X DELETE "/repos/${repo}/git/refs/tags/continuous"',
            '-f tag_name=continuous -f name="Continuous Build ${KLOGG_VERSION}"',
        ):
            self.assertIn(marker, critical)
        self.assertNotIn("\n      - name:", critical)
        self.assertNotIn("2>&1 || true", critical)
        reverify = section(
            workflow,
            "- name: Reverify promoted continuous release",
            "- name: Roll back failed continuous promotion",
        )
        self.assertIn("/branches/master", reverify)
        self.assertIn("KLOGG_SOURCE_COMMIT", reverify)
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

    def test_manifest_verifier_rejects_boolean_schema_versions(self):
        manifest, document = self.make_manifest()
        document["schema_version"] = True
        manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

        result = self.run_verifier(manifest)

        self.assertNotEqual(result.returncode, 0)
        self.assertRegex((result.stdout + result.stderr).lower(), r"schema|integer")

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

    def test_signed_evidence_requires_signing_and_notarization_in_qualification_policy(self):
        manifest, document = self.make_manifest("signed")
        qualification = self.root / "adb-helper-dmg-package-verification.json"
        receipt = json.loads(qualification.read_text(encoding="utf-8"))
        receipt["required_receipts"] = [
            "binary-build",
            "binary-smoke",
            "package-verification",
        ]
        receipt["verified_receipts"] = list(receipt["required_receipts"])
        qualification.write_text(
            json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8"
        )
        document["packages"][0]["qualification_receipt"]["sha256"] = sha256(
            qualification
        )
        manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

        result = self.run_verifier(manifest, "signed")

        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(
            (result.stdout + result.stderr).lower(), r"qualification.*sign|sign.*policy"
        )

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


class ConsolidatedReleasePublicationContractTest(unittest.TestCase):
    VERSION = "26.09.05.1701"
    COMMIT = "a" * 40
    BASE_URL = "https://github.com/ZEACENT/klogg"

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        self.contract = required_json(FIXTURE)
        self.release_contract = self.contract["release_publication"]

    def tearDown(self):
        self.tempdir.cleanup()

    def write_asset(self, name: str, content: bytes) -> pathlib.Path:
        path = self.root / name
        path.write_bytes(content)
        return path

    @staticmethod
    def json_bytes(document: object) -> bytes:
        return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()

    @staticmethod
    def write_deterministic_tar(
        path: pathlib.Path, members: list[tuple[str, bytes, bytes]]
    ) -> None:
        with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as archive:
            for name, content, member_type in members:
                info = tarfile.TarInfo(name)
                info.size = len(content) if member_type == tarfile.REGTYPE else 0
                info.type = member_type
                info.mode = 0o644
                info.uid = 0
                info.gid = 0
                info.mtime = 0
                info.uname = ""
                info.gname = ""
                if member_type == tarfile.SYMTYPE:
                    info.linkname = "index.json"
                    archive.addfile(info)
                else:
                    archive.addfile(info, io.BytesIO(content))

    def checksum_lines(self) -> list[str]:
        checksum_name = self.release_contract["checksums"]["file_name"]
        return (self.root / checksum_name).read_text(encoding="utf-8").splitlines()

    def refresh_checksums(self) -> None:
        checksum_name = self.release_contract["checksums"]["file_name"]
        names = sorted(
            path.name
            for path in self.root.iterdir()
            if path.is_file() and path.name != checksum_name
        )
        (self.root / checksum_name).write_text(
            "".join(f"{sha256(self.root / name)} *{name}\n" for name in names),
            encoding="utf-8",
        )

    def update_manifest(self, document: dict) -> pathlib.Path:
        manifest = self.root / self.contract["publication_manifest"]["file_name"]
        manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        self.refresh_checksums()
        return manifest

    def make_publication(
        self, channel: str = "stable", evidence_level: str = "signed"
    ) -> tuple[pathlib.Path, dict]:
        tag = f"v{self.VERSION}" if channel == "stable" else "continuous"
        public_name = (
            f"Release v{self.VERSION}"
            if channel == "stable"
            else f"Continuous Build {self.VERSION}"
        )
        support_labels = {
            "adb-helper-licenses.tar.gz": "ADB helper licenses",
            "adb-helper-notices.tar.gz": "ADB helper notices",
            "adb-helper-sbom.spdx.json": "ADB helper SBOM",
            "ADB-HELPER-SOURCE-OFFER.txt": "ADB helper source offer",
            "adb-helper-source-manifest.json": "ADB helper source manifest",
            "ios-native-source-offer.txt": "iOS native source offer",
            "ios-native-lgpl-replacement.txt": "iOS native LGPL replacement guide",
            "NOTICE-ios-native.txt": "iOS native notices",
        }
        for name in self.release_contract["top_level_support_assets"]:
            self.write_asset(name, f"original support bytes for {name}\n".encode())

        components = {}
        source_hashes = {}
        source_labels = {
            "adb-helper": "ADB helper corresponding source",
            "ios-native": "iOS native corresponding source",
        }
        receipt_labels = {
            "adb-helper": "ADB helper source-set receipt",
            "ios-native": "iOS native source-set receipt",
        }
        component_support = {
            "adb-helper": self.release_contract["top_level_support_assets"][:5],
            "ios-native": self.release_contract["top_level_support_assets"][5:],
        }
        for component in ("adb-helper", "ios-native"):
            archive_bytes = f"immutable {component} corresponding source\n".encode()
            archive_hash = hashlib.sha256(archive_bytes).hexdigest()
            archive_name = (
                f"klogg-v{self.VERSION}-{component}-source-{archive_hash[:12]}.tar.gz"
            )
            self.write_asset(archive_name, archive_bytes)
            source_set_name = f"{component}-source-set-receipt.json"
            source_set_document = {
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
                        "kind": "support",
                        "file_name": name,
                        "sha256": sha256(self.root / name),
                    }
                    for name in component_support[component]
                ],
                "distribution": {"package_required": False, "release_required": True},
            }
            source_set = self.write_asset(
                source_set_name, self.json_bytes(source_set_document)
            )
            source_hashes[component] = sha256(source_set)
            components[component] = {
                "display_name": source_labels[component],
                "source_set_receipt": {
                    "display_name": receipt_labels[component],
                    "file_name": source_set.name,
                    "sha256": source_hashes[component],
                },
                "source_archive": {
                    "file_name": archive_name,
                    "source_set_file_name": source_set_document["archive"]["file_name"],
                    "sha256": archive_hash,
                    "url": f"{self.BASE_URL}/releases/download/{tag}/{archive_name}",
                },
            }

        package_specs = [
            ("Windows", "Windows x64 installer (Qt 6, Vectorscan AVX2)"),
            ("Windows", "Windows x64 portable (Qt 6, Vectorscan AVX2)"),
            ("Linux", "Ubuntu 22.04 (Jammy, Vectorscan generic)"),
            ("Linux", "Ubuntu 24.04 (Noble, Vectorscan generic)"),
            ("Linux", "Ubuntu 26.04 (Resolute, Vectorscan generic)"),
            ("Linux", "AppImage (Vectorscan generic)"),
            ("macOS", "Intel (x64, Vectorscan generic)"),
            ("macOS", "Apple Silicon (ARM64, Vectorscan)"),
        ]
        package_names = [
            pattern.format(version=self.VERSION)
            for pattern in self.release_contract["packages"]
        ]
        self.assertEqual(
            len(package_names),
            len(package_specs),
            "release package fixture and display specifications must stay aligned",
        )
        packages = []
        for index, name in enumerate(package_names):
            group, label = package_specs[index]
            package = self.write_asset(name, f"qualified package bytes for {name}\n".encode())
            packages.append(
                {
                    "file_name": name,
                    "sha256": sha256(package),
                    "display": {"section": group, "label": label},
                    "source_sets": {
                        "adb-helper": source_hashes["adb-helper"],
                        **(
                            {"ios-native": source_hashes["ios-native"]}
                            if name.endswith(".dmg")
                            else {}
                        ),
                    },
                    "evidence": {},
                }
            )

        receipt_members: dict[str, bytes] = {}
        references: list[dict] = []

        def add_evidence(
            package: dict,
            kind: str,
            source_name: str,
            document: dict,
        ) -> None:
            content = self.json_bytes(document)
            digest = hashlib.sha256(content).hexdigest()
            member = f"receipts/{digest}.json"
            receipt_members.setdefault(member, content)
            reference = {
                "package": package["file_name"],
                "kind": kind,
                "source_file_name": source_name,
                "member": member,
                "sha256": digest,
            }
            package["evidence"][kind] = {
                "member": member,
                "sha256": digest,
            }
            references.append(reference)

        windows_receipt = {
            "receipt_kind": "package-verification",
            "target": "windows-x64",
            "release_qualified": True,
            "source_set_receipt_sha256": source_hashes["adb-helper"],
            "required_receipts": ["binary-build", "binary-smoke", "package-verification"],
            "verified_receipts": ["binary-build", "binary-smoke", "package-verification"],
            "packages": [
                {"name": package["file_name"], "sha256": package["sha256"]}
                for package in packages[:2]
            ],
        }
        for package in packages[:2]:
            add_evidence(
                package,
                "qualification",
                "adb-helper-windows-package-verification.json",
                windows_receipt,
            )

        for package in packages[2:]:
            target = package["display"]["section"].lower()
            qualification = {
                "receipt_kind": "package-verification",
                "target": target,
                "release_qualified": evidence_level == "signed" or not package["file_name"].endswith(".dmg"),
                "source_set_receipt_sha256": source_hashes["adb-helper"],
                "source_helper_sha256": "6" * 64,
                "helper_sha256": "7" * 64,
                "required_receipts": [
                    "binary-build",
                    "binary-smoke",
                    "package-verification",
                    *(["signing", "notarization"] if package["file_name"].endswith(".dmg") else []),
                ],
                "verified_receipts": [
                    "binary-build",
                    "binary-smoke",
                    "package-verification",
                    *(
                        ["signing", "notarization"]
                        if evidence_level == "signed" and package["file_name"].endswith(".dmg")
                        else []
                    ),
                ],
                "packages": [
                    {"name": package["file_name"], "sha256": package["sha256"]}
                ],
            }
            add_evidence(
                package,
                "qualification",
                f"{package['file_name']}.qualification.json",
                qualification,
            )
            if not package["file_name"].endswith(".dmg"):
                continue
            ios_package = {
                "schema_version": 2,
                "receipt_kind": "ios-native-package",
                "status": "passed",
                "source_set_receipt_sha256": source_hashes["ios-native"],
                "architecture": "arm64" if "arm64" in package["file_name"] else "x64",
                "dylibs": [],
            }
            ios_package_bytes = self.json_bytes(ios_package)
            ios_package_hash = hashlib.sha256(ios_package_bytes).hexdigest()
            ios_qualification = {
                "receipt_kind": "ios-native-package-verification",
                "qualification": evidence_level,
                "release_qualified": evidence_level == "signed",
                "source_set_receipt_sha256": source_hashes["ios-native"],
                "ios_native_package_receipt_sha256": ios_package_hash,
                "packages": [
                    {"name": package["file_name"], "sha256": package["sha256"]}
                ],
            }
            add_evidence(
                package,
                "ios-package",
                "ios-native-package-receipt.json",
                ios_package,
            )
            add_evidence(
                package,
                "ios-qualification",
                f"{package['file_name']}.ios-qualification.json",
                ios_qualification,
            )
            if evidence_level == "signed":
                add_evidence(
                    package,
                    "signing",
                    "adb-helper-signing-receipt.json",
                    {
                        "receipt_kind": "signing",
                        "status": "passed",
                        "target": target,
                        "identity": "Developer ID Application: Klogg Test",
                        "package_sha256": package["sha256"],
                        "source_helper_sha256": "6" * 64,
                        "signed_helper_sha256": "7" * 64,
                    },
                )
                add_evidence(
                    package,
                    "notarization",
                    "adb-helper-notarization-receipt.json",
                    {
                        "receipt_kind": "notarization",
                        "status": "passed",
                        "target": target,
                        "team_id": "TESTTEAM123",
                        "submission_id": "00000000-0000-0000-0000-000000000001",
                        "package_sha256": package["sha256"],
                        "source_helper_sha256": "6" * 64,
                        "signed_helper_sha256": "7" * 64,
                    },
                )

        # Preserve the exact input bytes so the archive contract can prove that
        # publication did not parse/re-serialize receipts before bundling them.
        self.original_receipt_bytes = dict(receipt_members)
        index = {
            "schema_version": self.release_contract["evidence_archive"]["schema_version"],
            "index_kind": self.release_contract["evidence_archive"]["index_kind"],
            "receipts": [
                {
                    "member": member,
                    "sha256": hashlib.sha256(content).hexdigest(),
                    "size": len(content),
                }
                for member, content in sorted(receipt_members.items())
            ],
            "references": sorted(
                references,
                key=lambda item: (
                    item["package"], item["kind"], item["source_file_name"]
                ),
            ),
        }
        index_bytes = self.json_bytes(index)
        evidence_name = self.release_contract["evidence_archive"]["file_name"]
        evidence = self.root / evidence_name
        self.write_deterministic_tar(
            evidence,
            [
                (
                    self.release_contract["evidence_archive"]["index_member"],
                    index_bytes,
                    tarfile.REGTYPE,
                ),
                *[
                    (member, content, tarfile.REGTYPE)
                    for member, content in sorted(receipt_members.items())
                ],
            ],
        )

        document = {
            "schema_version": self.contract["publication_manifest"]["schema_version"],
            "manifest_kind": self.contract["publication_manifest"]["manifest_kind"],
            "channel": channel,
            "evidence_level": evidence_level,
            "release": {
                "tag": tag,
                "version": self.VERSION,
                "commit": self.COMMIT,
                "mutable": channel == "continuous",
                "public_name": public_name,
                "page_url": f"{self.BASE_URL}/releases/tag/{tag}",
                "direct_asset_urls_are_archival": channel == "stable",
            },
            "components": components,
            "packages": packages,
            "support_assets": [
                {
                    "display_name": support_labels[name],
                    "file_name": name,
                    "sha256": sha256(self.root / name),
                }
                for name in self.release_contract["top_level_support_assets"]
            ],
            "evidence_archive": {
                "file_name": evidence_name,
                "sha256": sha256(evidence),
                "index_member": self.release_contract["evidence_archive"]["index_member"],
                "index_sha256": hashlib.sha256(index_bytes).hexdigest(),
            },
            "checksums": self.release_contract["checksums"],
        }
        manifest = self.root / self.contract["publication_manifest"]["file_name"]
        manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        self.refresh_checksums()
        return manifest, document

    def run_verifier(
        self,
        manifest: pathlib.Path,
        channel: str = "stable",
        evidence_level: str = "signed",
    ) -> subprocess.CompletedProcess[str]:
        tag = f"v{self.VERSION}" if channel == "stable" else "continuous"
        return subprocess.run(
            [
                sys.executable,
                str(PUBLICATION_VERIFIER),
                "--manifest",
                str(manifest),
                "--assets-root",
                str(self.root),
                "--expected-channel",
                channel,
                "--expected-evidence-level",
                evidence_level,
                "--expected-tag",
                tag,
                "--expected-version",
                self.VERSION,
                "--expected-commit",
                self.COMMIT,
                "--expected-base-url",
                self.BASE_URL,
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def read_tar_members(self) -> list[tuple[str, bytes, bytes]]:
        evidence = self.root / self.release_contract["evidence_archive"]["file_name"]
        members = []
        with tarfile.open(evidence, "r:") as archive:
            for info in archive.getmembers():
                stream = archive.extractfile(info) if info.isfile() else None
                members.append((info.name, stream.read() if stream else b"", info.type))
        return members

    def replace_evidence_members(
        self,
        manifest: pathlib.Path,
        document: dict,
        members: list[tuple[str, bytes, bytes]],
        *,
        synchronize_index_hash: bool = False,
    ) -> None:
        evidence = self.root / self.release_contract["evidence_archive"]["file_name"]
        self.write_deterministic_tar(evidence, members)
        document["evidence_archive"]["sha256"] = sha256(evidence)
        if synchronize_index_hash:
            index_name = self.release_contract["evidence_archive"]["index_member"]
            matching = [content for name, content, _ in members if name == index_name]
            if len(matching) == 1:
                document["evidence_archive"]["index_sha256"] = hashlib.sha256(
                    matching[0]
                ).hexdigest()
        self.update_manifest(document)

    def expected_body(self, document: dict, changelog: str) -> str:
        release = document["release"]
        tag = release["tag"]

        def link(label: str, name: str) -> str:
            return f"- [{label}]({self.BASE_URL}/releases/download/{tag}/{name})"

        if document["channel"] == "stable":
            introduction = (
                "This immutable release was verified from its publication manifest."
            )
        else:
            introduction = (
                f"The [continuous release page]({self.BASE_URL}/releases/tag/continuous) is durable. "
                "The versioned asset links below identify only the current build; they are replaced "
                "by the next successful publication and are not archival permalinks."
            )
        lines = [release["public_name"], "", introduction, "", "## Changes", changelog, "", "## Downloads"]
        for group in ("Windows", "Linux", "macOS"):
            lines.extend(["", f"### {group}"])
            if group == "macOS":
                lines.append(
                    "Signed evidence includes signing and notarization receipts."
                    if document["evidence_level"] == "signed"
                    else "These disk images are unsigned validation artifacts, not signed or notarized releases."
                )
            lines.extend(
                link(package["display"]["label"], package["file_name"])
                for package in document["packages"]
                if package["display"]["section"] == group
            )
        lines.extend(["", "### Corresponding source and legal materials"])
        for component in ("adb-helper", "ios-native"):
            record = document["components"][component]
            lines.append(link(record["display_name"], record["source_archive"]["file_name"]))
            receipt = record["source_set_receipt"]
            lines.append(link(receipt["display_name"], receipt["file_name"]))
        lines.extend(
            link(asset["display_name"], asset["file_name"])
            for asset in document["support_assets"]
        )
        lines.extend(
            [
                "",
                "### Verification",
                link("Publication manifest", self.contract["publication_manifest"]["file_name"]),
                link("Release evidence", self.release_contract["evidence_archive"]["file_name"]),
                link("SHA-256 checksums", self.release_contract["checksums"]["file_name"]),
                "",
            ]
        )
        return "\n".join(lines)

    def test_package_fixture_and_display_specs_must_stay_aligned(self):
        self.release_contract["packages"] = [
            *self.release_contract["packages"],
            "klogg-{version}-unexpected.pkg",
        ]

        with self.assertRaisesRegex(
            AssertionError,
            r"package fixture and display specifications must stay aligned",
        ):
            self.make_publication()

    def test_target_publication_has_exact_23_uploaded_and_25_visible_assets(self):
        manifest, document = self.make_publication()
        uploaded = sorted(path.name for path in self.root.iterdir())
        package_names = {package["file_name"] for package in document["packages"]}
        source_archives = {
            component["source_archive"]["file_name"]
            for component in document["components"].values()
        }
        source_receipts = {
            component["source_set_receipt"]["file_name"]
            for component in document["components"].values()
        }
        support_names = {asset["file_name"] for asset in document["support_assets"]}
        expected = package_names | source_archives | source_receipts | support_names | {
            manifest.name,
            self.release_contract["evidence_archive"]["file_name"],
            self.release_contract["checksums"]["file_name"],
        }
        self.assertEqual(set(uploaded), expected)
        self.assertEqual(len(uploaded), 23, uploaded)
        self.assertEqual(len(package_names), 8)
        self.assertEqual(len(source_archives), 2)
        self.assertEqual(len(source_receipts), 2)
        self.assertEqual(len(support_names), 8)
        self.assertEqual(
            len(uploaded) + self.release_contract["github_generated_asset_count"], 25
        )
        checksum_lines = self.checksum_lines()
        self.assertEqual(
            len(checksum_lines),
            self.release_contract["checksums"]["covered_asset_count"],
        )
        self.assertEqual(
            checksum_lines,
            sorted(checksum_lines, key=lambda line: line.split(" *", 1)[1]),
        )
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{64} \*[^/]+", line) for line in checksum_lines))
        self.assertIn(manifest.name, uploaded)
        self.assertFalse(any(name.endswith(".sha256") for name in uploaded))
        self.assertFalse(any("qualification.json" in name for name in uploaded))

    def test_manifest_v2_accepts_consolidated_checksums_and_evidence(self):
        manifest, _ = self.make_publication()
        result = self.run_verifier(manifest)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_must_be_the_exact_checksummed_asset_inside_the_publication_root(self):
        manifest, _ = self.make_publication()
        outside = self.root / "outside"
        outside.mkdir()
        detached_manifest = outside / manifest.name
        detached_manifest.write_bytes(manifest.read_bytes())

        result = self.run_verifier(detached_manifest)

        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(
            (result.stdout + result.stderr).lower(), r"manifest.*assets root|publication root"
        )

    def test_sha256sums_rejects_duplicate_traversal_malformed_missing_tampered_and_unlisted_assets(self):
        scenarios = (
            "duplicate",
            "traversal",
            "malformed",
            "missing",
            "tampered",
            "unlisted",
        )
        expected = {
            "duplicate": r"duplicate.*checksum",
            "traversal": r"unsafe.*checksum|checksum.*path",
            "malformed": r"malformed.*checksum|checksum.*format",
            "missing": r"missing.*checksum|checksum.*coverage",
            "tampered": r"hash mismatch|checksum mismatch",
            "unlisted": r"unlisted|unreferenced|checksum.*coverage",
        }
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                manifest, _ = self.make_publication()
                checksums = self.root / self.release_contract["checksums"]["file_name"]
                lines = checksums.read_text(encoding="utf-8").splitlines()
                if scenario == "duplicate":
                    lines.append(lines[0])
                elif scenario == "traversal":
                    lines.append(f"{'0' * 64} *../escape")
                elif scenario == "malformed":
                    lines.append("not-a-sha256sum-record")
                elif scenario == "missing":
                    lines.pop(0)
                elif scenario == "tampered":
                    package_name = self.release_contract["packages"][0].format(
                        version=self.VERSION
                    )
                    (self.root / package_name).write_bytes(b"tampered package\n")
                else:
                    self.write_asset("unlisted-release-asset.txt", b"not in SHA256SUMS\n")
                if scenario in {"duplicate", "traversal", "malformed", "missing"}:
                    checksums.write_text("\n".join(lines) + "\n", encoding="utf-8")
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex((result.stdout + result.stderr).lower(), expected[scenario])
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

    def test_evidence_tar_rejects_duplicate_unsafe_type_hash_index_missing_and_extra_members(self):
        scenarios = (
            "duplicate",
            "duplicate-content",
            "unsafe",
            "type",
            "hash",
            "malformed-index",
            "boolean-index-schema",
            "missing-index",
            "missing-receipt",
            "extra",
        )
        expected = {
            "duplicate": r"duplicate.*evidence|duplicate.*member",
            "duplicate-content": r"duplicate.*content|content.*identity|deduplicat|content.address",
            "unsafe": r"unsafe.*evidence|unsafe.*member",
            "type": r"evidence.*type|regular file",
            "hash": r"evidence.*hash|receipt.*hash",
            "malformed-index": r"invalid.*evidence.*index|malformed.*index",
            "boolean-index-schema": r"evidence.*index.*schema|schema.*integer",
            "missing-index": r"missing.*evidence.*index",
            "missing-receipt": r"missing.*receipt|index.*missing",
            "extra": r"extra.*evidence|unreferenced.*receipt|index.*coverage",
        }
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                manifest, document = self.make_publication()
                members = self.read_tar_members()
                index_name = self.release_contract["evidence_archive"]["index_member"]
                receipt_index = next(
                    index for index, member in enumerate(members) if member[0].startswith("receipts/")
                )
                synchronize_index_hash = False
                if scenario == "duplicate":
                    members.append(members[receipt_index])
                elif scenario == "duplicate-content":
                    source_name, source_content, _ = members[receipt_index]
                    duplicate_name = "receipts/duplicate-content.json"
                    index_position = next(
                        index for index, member in enumerate(members) if member[0] == index_name
                    )
                    index_document = json.loads(members[index_position][1])
                    source_record = next(
                        record for record in index_document["receipts"]
                        if record["member"] == source_name
                    )
                    duplicate_record = copy.deepcopy(source_record)
                    duplicate_record["member"] = duplicate_name
                    index_document["receipts"].append(duplicate_record)
                    index_document["receipts"].sort(key=lambda record: record["member"])
                    index_document["references"].append(
                        {
                            "package": document["packages"][0]["file_name"],
                            "kind": "duplicate-content-probe",
                            "source_file_name": "duplicate-content.json",
                            "member": duplicate_name,
                            "sha256": source_record["sha256"],
                        }
                    )
                    index_content = self.json_bytes(index_document)
                    members[index_position] = (index_name, index_content, tarfile.REGTYPE)
                    members.append((duplicate_name, source_content, tarfile.REGTYPE))
                    synchronize_index_hash = True
                elif scenario == "unsafe":
                    members.append(("../escape.json", b"{}\n", tarfile.REGTYPE))
                elif scenario == "type":
                    members.append(("receipts/link.json", b"", tarfile.SYMTYPE))
                elif scenario == "hash":
                    name, _, member_type = members[receipt_index]
                    members[receipt_index] = (name, b"tampered receipt\n", member_type)
                elif scenario == "malformed-index":
                    members = [
                        (name, b"{not-json\n" if name == index_name else content, member_type)
                        for name, content, member_type in members
                    ]
                    synchronize_index_hash = True
                elif scenario == "boolean-index-schema":
                    index_position = next(
                        index
                        for index, member in enumerate(members)
                        if member[0] == index_name
                    )
                    index_document = json.loads(members[index_position][1])
                    index_document["schema_version"] = True
                    members[index_position] = (
                        index_name,
                        self.json_bytes(index_document),
                        tarfile.REGTYPE,
                    )
                    synchronize_index_hash = True
                elif scenario == "missing-index":
                    members = [member for member in members if member[0] != index_name]
                elif scenario == "missing-receipt":
                    members.pop(receipt_index)
                else:
                    members.append(("receipts/extra.json", b"{}\n", tarfile.REGTYPE))
                self.replace_evidence_members(
                    manifest,
                    document,
                    members,
                    synchronize_index_hash=synchronize_index_hash,
                )
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex((result.stdout + result.stderr).lower(), expected[scenario])
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

    def test_windows_setup_and_portable_share_one_content_identity_receipt_member(self):
        manifest, document = self.make_publication()
        setup_ref = document["packages"][0]["evidence"]["qualification"]
        portable_ref = document["packages"][1]["evidence"]["qualification"]
        self.assertEqual(setup_ref, portable_ref)
        with tarfile.open(
            self.root / self.release_contract["evidence_archive"]["file_name"], "r:"
        ) as archive:
            self.assertEqual(
                [member.name for member in archive.getmembers()].count(setup_ref["member"]),
                1,
            )
        result = self.run_verifier(manifest)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_evidence_archive_preserves_every_original_receipt_byte_and_is_deterministic(self):
        manifest, _ = self.make_publication()
        evidence_path = self.root / self.release_contract["evidence_archive"]["file_name"]
        first_archive = evidence_path.read_bytes()
        with tarfile.open(evidence_path, "r:") as archive:
            members = {member.name: member for member in archive.getmembers()}
            self.assertEqual(len(members), len(archive.getmembers()))
            for member_name, original_bytes in self.original_receipt_bytes.items():
                self.assertIn(member_name, members)
                stream = archive.extractfile(members[member_name])
                self.assertIsNotNone(stream)
                self.assertEqual(stream.read(), original_bytes)
            for member in members.values():
                self.assertTrue(member.isfile())
                self.assertEqual(member.mode, 0o644)
                self.assertEqual(member.uid, 0)
                self.assertEqual(member.gid, 0)
                self.assertEqual(member.mtime, 0)
                self.assertEqual(member.uname, "")
                self.assertEqual(member.gname, "")

        for path in list(self.root.iterdir()):
            if path.is_file():
                path.unlink()
        self.make_publication()
        self.assertEqual(evidence_path.read_bytes(), first_archive)
        result = self.run_verifier(manifest)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_rejects_numeric_release_booleans(self):
        for field, numeric_value in (
            ("mutable", 0),
            ("direct_asset_urls_are_archival", 1),
        ):
            with self.subTest(field=field):
                manifest, document = self.make_publication()
                document["release"][field] = numeric_value
                self.update_manifest(document)

                result = self.run_verifier(manifest)

                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(), r"boolean|release"
                )
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

    def test_manifest_rejects_unknown_incomplete_or_extended_schema_and_public_candidate_name(self):
        schema_mutations = {
            "unknown-version": lambda document: document.update(schema_version=3),
            "incomplete": lambda document: document.pop("checksums"),
            "unknown-field": lambda document: document.update(unrecognized_release_field=True),
        }
        for scenario, mutate in schema_mutations.items():
            with self.subTest(scenario=scenario):
                manifest, document = self.make_publication()
                mutate(document)
                self.update_manifest(document)
                result = self.run_verifier(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(),
                    r"unsupported.*schema|invalid.*schema|schema.*field|checksums",
                )
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

        manifest, document = self.make_publication()
        document["release"]["public_name"] = f"Continuous Candidate {self.VERSION}"
        self.update_manifest(document)
        candidate_result = self.run_verifier(manifest)
        self.assertNotEqual(candidate_result.returncode, 0)
        self.assertRegex(
            (candidate_result.stdout + candidate_result.stderr).lower(),
            r"candidate.*public|public.*candidate",
        )

    def test_renderer_emits_exact_verified_stable_and_continuous_bodies(self):
        changelog = "- Fix publication inventory\n- Consolidate release evidence"
        for channel, evidence_level in (("stable", "signed"), ("continuous", "validation")):
            with self.subTest(channel=channel):
                manifest, document = self.make_publication(channel, evidence_level)
                render_root = self.root / "render-inputs"
                render_root.mkdir(exist_ok=True)
                changelog_path = render_root / "CHANGELOG.txt"
                output = render_root / "release-body.md"
                changelog_path.write_text(changelog + "\n", encoding="utf-8")
                result = subprocess.run(
                    [
                        sys.executable,
                        str(RELEASE_DOWNLOAD_RENDERER),
                        "--manifest",
                        str(manifest),
                        "--assets-root",
                        str(self.root),
                        "--changelog-file",
                        str(changelog_path),
                        "--output",
                        str(output),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                body = output.read_text(encoding="utf-8")
                self.assertEqual(body, self.expected_body(document, changelog))
                urls = re.findall(r"\]\(([^)]+)\)", body)
                self.assertEqual(len(urls), 23 + (channel == "continuous"))
                for url in urls:
                    parsed = urllib.parse.urlsplit(url)
                    self.assertEqual(parsed.scheme, "https")
                    self.assertEqual(parsed.netloc, "github.com")
                    self.assertEqual(
                        urllib.parse.quote(
                            urllib.parse.unquote(parsed.path), safe="/-._~"
                        ),
                        parsed.path,
                        f"release URL path is not canonically encoded: {url}",
                    )
                self.assertNotIn("{version}", body)
                self.assertNotIn("${KLOGG_VERSION}", body)
                if channel == "continuous":
                    body = output.read_text(encoding="utf-8")
                    self.assertIn(f"{self.BASE_URL}/releases/tag/continuous", body)
                    self.assertIn("not archival permalinks", body)
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

    def test_renderer_fails_closed_for_near_miss_spoof_malformed_unverified_and_channel_mismatch(self):
        scenarios = (
            "near-miss",
            "string-spoof",
            "malformed",
            "tampered-package",
            "channel-mismatch",
        )
        expected = {
            "near-miss": r"page.*url|continuous",
            "string-spoof": r"page.*url|repository|schema.*field|unknown.*field",
            "malformed": r"manifest|json",
            "tampered-package": r"hash|checksum|verified",
            "channel-mismatch": r"channel|tag",
        }
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                manifest, document = self.make_publication("continuous", "validation")
                if scenario == "near-miss":
                    document["release"]["page_url"] = (
                        f"{self.BASE_URL}/releases/tag/continuous-candidate"
                    )
                    self.update_manifest(document)
                elif scenario == "string-spoof":
                    document["note"] = (
                        f"safe durable URL: {self.BASE_URL}/releases/tag/continuous"
                    )
                    document["release"]["page_url"] = "https://example.invalid/spoof"
                    self.update_manifest(document)
                elif scenario == "malformed":
                    manifest.write_text("{not-json\n", encoding="utf-8")
                elif scenario == "tampered-package":
                    package = self.root / document["packages"][0]["file_name"]
                    package.write_bytes(package.read_bytes() + b"tampered\n")
                else:
                    document["channel"] = "stable"
                    self.update_manifest(document)
                render_root = self.root / "render-inputs"
                render_root.mkdir(exist_ok=True)
                changelog = render_root / "CHANGELOG.txt"
                changelog.write_text("- Consolidate release downloads\n", encoding="utf-8")
                output = render_root / "release-body.md"
                if output.exists():
                    output.unlink()
                result = subprocess.run(
                    [
                        sys.executable,
                        str(RELEASE_DOWNLOAD_RENDERER),
                        "--manifest",
                        str(manifest),
                        "--assets-root",
                        str(self.root),
                        "--changelog-file",
                        str(changelog),
                        "--output",
                        str(output),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(), expected[scenario]
                )
                self.assertFalse(output.exists())
                for path in list(self.root.iterdir()):
                    if path.is_file():
                        path.unlink()

    def test_workflows_stage_only_consolidated_assets_and_use_rendered_body(self):
        stable = required_text(CI_RELEASE)
        continuous = required_text(CI_CONTINUOUS)
        for workflow in (stable, continuous):
            self.assertIn("render_release_downloads.py", workflow)
            self.assertIn("body_path:", workflow)
            self.assertIn(self.release_contract["evidence_archive"]["file_name"], workflow)
            self.assertIn(self.release_contract["checksums"]["file_name"], workflow)
            self.assertNotRegex(workflow, r"packages-publication/[^\n]*\.sha256")
            self.assertNotRegex(
                workflow,
                r"packages-publication/[^\n]*(?:qualification|signing|notarization)\.json",
            )
        self.assertNotIn("Candidate", section(continuous, "- name: Create continuous candidate draft", "- name: Verify continuous candidate after upload").split("name:", 2)[-1])
        self.assertIn('-f name="Continuous Build ${KLOGG_VERSION}"', continuous)
        self.assertEqual(
            self.release_contract["uploaded_asset_count"],
            8 + 2 + 2 + 8 + 1 + 1 + 1,
        )

    def test_stable_release_has_no_dead_legacy_package_bin_or_debug_repackaging(self):
        stable = required_text(CI_RELEASE)
        publication = section(
            stable,
            "- name: Generate Changelog",
            "- name: Clean stale stable draft",
        )
        for obsolete in (
            "packages-bin",
            "linux-debug",
            "klogg_deps.tar.xz",
            "source-publication-sha256.txt",
            "adb-helper-release-sha256.txt",
        ):
            self.assertNotIn(obsolete, publication)
        changelog = section(
            stable,
            "- name: Generate Changelog",
            "- name: Display structure of downloaded files",
        )
        self.assertIn("release-changelog.txt", changelog)
        self.assertNotIn("GITHUB_ENV", changelog)
        self.assertNotIn("dd if=/dev/urandom", changelog)

    def test_preparer_emits_exactly_one_checksum_and_one_evidence_archive_without_sidecars(self):
        preparer = required_text(PUBLICATION_PREPARER)
        self.assertIn('"schema_version": 2', preparer)
        self.assertIn('"manifest_kind": "klogg-release-publication"', preparer)
        self.assertEqual(preparer.count('"klogg-release-evidence.tar"'), 1)
        self.assertEqual(preparer.count('"SHA256SUMS"'), 1)
        self.assertNotRegex(
            preparer,
            r"output\s*/\s*f?[\"'][^\"']*\.sha256",
        )
        for suffix in (
            ".qualification.json",
            ".ios-qualification.json",
            ".ios-package.json",
            ".signing.json",
            ".notarization.json",
        ):
            self.assertNotIn(suffix, preparer)


if __name__ == "__main__":
    unittest.main()
