import importlib.util
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "lint_ci_quality.py"
SPEC = importlib.util.spec_from_file_location("lint_ci_quality", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


PINNED = "3d3c42e5aac5ba805825da76410c181273ba90b1"
NODE20_CHECKOUT_PINNED = "11d5960a326750d5838078e36cf38b85af677262"
CACHE_PINNED = "55cc8345863c7cc4c66a329aec7e433d2d1c52a9"
UPLOAD_ARTIFACT_PINNED = "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a"
DOWNLOAD_ARTIFACT_PINNED = "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c"
CODEQL_PINNED = "cdf488f595d80d6e07e03d4674febd5ab45fa938"
NODE20_CODEQL_PINNED = "4187e74d05793876e9989daffde9c3e66b4acd07"


def secure_codeql_workflow() -> str:
    return f"""\
on:
  push:
    branches: [master]
  pull_request:
    branches: [master]
  workflow_dispatch:
jobs:
  analyze:
    permissions:
      contents: read
      security-events: write
    env:
      CODEQL_ACTION_OVERLAY_ANALYSIS: "false"
      CODEQL_ACTION_OVERLAY_ANALYSIS_CODE_SCANNING_CPP: "false"
    timeout-minutes: 30
    steps:
      - uses: github/codeql-action/init@{CODEQL_PINNED}
        with:
          languages: c-cpp
          build-mode: manual
      - uses: ./.github/actions/agent-setup
      - run: cmake -S "$GITHUB_WORKSPACE" -B build -DCPM_SOURCE_CACHE="$GITHUB_WORKSPACE/cpm_cache" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      - run: cmake --build build -t klogg
      - uses: github/codeql-action/analyze@{CODEQL_PINNED} # v4.37.9
"""


class CiQualityLintTest(unittest.TestCase):
    def test_checkout_requires_sha_and_disabled_credentials(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n  - uses: actions/checkout@v4\n  - run: true\n",
        )
        self.assertEqual(len(issues), 2)

    def test_secure_checkout_is_accepted(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@{PINNED} # reviewed version\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(issues, [])

    def test_comments_cannot_spoof_checkout_security(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@v4 # actions/checkout@{PINNED}\n"
            "    with:\n"
            "      # persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 2)

    def test_block_scalar_checkout_is_still_validated(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  - uses: >-\n"
            "      actions/checkout@v4\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("40-char SHA", issues[0])

    def test_block_scalar_cannot_spoof_checkout_credentials(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@{PINNED}\n"
            "    with:\n"
            "      sparse-checkout: |\n"
            "        persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("persist-credentials", issues[0])

    def test_bare_dash_checkout_step_is_parsed(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  -\n"
            "    uses: actions/checkout@v4\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("40-char SHA", issues[0])

    def test_named_checkout_step_is_parsed(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  - name: Checkout\n"
            f"    uses: actions/checkout@{PINNED}\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(issues, [])

    def test_all_remote_actions_require_reviewed_commit_shas(self):
        path = pathlib.Path(".github/workflows/test.yml")
        bad_refs = (
            "actions/cache@v5",
            "actions/cache@v5 # actions/cache@" + CACHE_PINNED,
            ">-\n      actions/cache@v5",
        )
        for action_ref in bad_refs:
            with self.subTest(action_ref=action_ref):
                issues = MODULE.check_checkout_blocks(
                    path,
                    "steps:\n  - uses: " + action_ref + "\n",
                )
                self.assertTrue(
                    any("remote actions must use a reviewed 40-char SHA" in issue for issue in issues),
                    issues,
                )

    def test_sha_pinned_but_unreviewed_remote_action_is_rejected(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            f"steps:\n  - uses: actions/cache@{'a' * 40}\n",
        )
        self.assertTrue(
            any("remote action SHA is not the reviewed revision" in issue for issue in issues),
            issues,
        )

    def test_current_remote_action_sha_and_local_actions_are_accepted(self):
        path = pathlib.Path(".github/workflows/test.yml")
        text = (
            "steps:\n"
            f"  - uses: actions/cache@{CACHE_PINNED}\n"
            f"  - uses: actions/upload-artifact@{UPLOAD_ARTIFACT_PINNED}\n"
            f"  - uses: actions/download-artifact@{DOWNLOAD_ARTIFACT_PINNED}\n"
            "  - uses: ./.github/actions/agent-setup\n"
        )
        self.assertEqual(MODULE.check_checkout_blocks(path, text), [])

    def test_known_node20_action_generations_are_rejected_even_when_sha_pinned(self):
        path = pathlib.Path(".github/workflows/test.yml")
        node20_actions = (
            (
                f"actions/checkout@{NODE20_CHECKOUT_PINNED}",
                "    with:\n      persist-credentials: false\n",
            ),
            (
                "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
                "",
            ),
            (
                "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093",
                "",
            ),
            (
                "ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756",
                "",
            ),
        )
        for action_ref, suffix in node20_actions:
            with self.subTest(action_ref=action_ref):
                issues = MODULE.check_checkout_blocks(
                    path,
                    f"steps:\n  - uses: {action_ref}\n{suffix}",
                )
                self.assertTrue(
                    any("known Node20 action generation" in issue for issue in issues),
                    issues,
                )

    def test_workflow_job_needs_parses_inline_and_block_lists(self):
        workflow = """\
jobs:
  Root:
    runs-on: ubuntu-24.04
  Inline:
    needs: [Root, 'Other'] # direct prerequisites
    runs-on: ubuntu-24.04
  Block:
    needs:
      - Root
      - "Inline"
    strategy:
      matrix:
        needs: [NotAJobDependency]
    steps:
      - run: echo 'needs: [AlsoNotAJobDependency]'
  Other:
    runs-on: ubuntu-24.04
"""
        self.assertEqual(
            MODULE.workflow_job_needs(workflow),
            {
                "Root": set(),
                "Inline": {"Root", "Other"},
                "Block": {"Root", "Inline"},
                "Other": set(),
            },
        )
        self.assertEqual(
            MODULE.workflow_job_ancestors(MODULE.workflow_job_needs(workflow), "Block"),
            {"Root", "Inline", "Other"},
        )

    def test_workflow_job_ancestors_rejects_cycles_and_unknown_jobs(self):
        with self.assertRaisesRegex(ValueError, "dependency cycle"):
            MODULE.workflow_job_ancestors({"One": {"Two"}, "Two": {"One"}}, "One")
        with self.assertRaisesRegex(ValueError, "unknown job Missing"):
            MODULE.workflow_job_ancestors({"One": {"Missing"}}, "One")

    def test_workflow_artifact_actions_extracts_names_from_action_steps(self):
        workflow = f"""\
jobs:
  Producer:
    steps:
      - name: Upload source closure
        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02
        with:
          name: locked-sources
          path: sources
  Consumer:
    needs: Producer
    steps:
      - uses: actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093
        with:
          name: locked-sources
          path: sources
"""
        self.assertEqual(
            MODULE.workflow_artifact_actions(workflow),
            {
                "Producer": {"uploads": {"locked-sources"}, "downloads": set()},
                "Consumer": {"uploads": set(), "downloads": {"locked-sources"}},
            },
        )

    def test_master_pushes_never_skip_ci_build_by_path(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        self.assertNotIn(
            "master pushes must not skip CI Build by path",
            MODULE.ci_build_workflow_issues(workflow),
        )
        mutated = workflow.replace(
            "  push:\n    branches: [ master ]\n",
            "  push:\n    branches: [ master ]\n    paths-ignore: [README.md]\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "master pushes must not skip CI Build by path",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_uses_the_optimized_prefetch_and_native_build_dag(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        self.assertEqual(MODULE.ci_build_workflow_issues(workflow), [])

    def test_ci_build_native_artifact_consumers_use_verified_extraction(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        linux = workflow.split("  LinuxPackages:", 1)[1].split(
            "\n  LinuxSanitizers:", 1
        )[0]
        mac = workflow.split("  MacPackages:", 1)[1].split(
            "\n  MacArmPackages:", 1
        )[0]
        for block in (linux, mac):
            self.assertIn("scripts/extract_verified_tar.py", block)
            self.assertNotIn('tar -xzf "$archive"', block)

    def test_ci_build_splits_package_and_package_free_platform_jobs(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        needs = MODULE.workflow_job_needs(workflow)
        expected_package_free = {
            "LinuxSanitizers": {
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchCmakeInstaller",
            },
            "LinuxTsan": {"SaveVersion", "PrefetchCpmCache"},
            "MacSanitizers": {"SaveVersion", "PrefetchCpmCache", "PrefetchBoost"},
            "WindowsX86": {
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchBoost",
                "PrefetchOpenSsl",
                "PrefetchWindowsTools",
            },
            "WindowsAsan": {
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchBoost",
                "PrefetchWindowsTools",
            },
        }
        mobile_jobs = {
            "BuildAdbHelpers",
            "BuildAdbLinuxArm64",
            "BuildAdbWindowsX64",
            "BuildAdbMacX64",
            "BuildAdbMacArm64",
            "BuildIosNativeStacks",
            "BuildIosNativeArm64",
        }
        for job, expected_needs in expected_package_free.items():
            with self.subTest(job=job):
                self.assertEqual(needs.get(job), expected_needs)
                self.assertTrue(
                    MODULE.workflow_job_ancestors(needs, job).isdisjoint(mobile_jobs)
                )

        expected_package_jobs = {
            "LinuxPackages": {
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchLinuxDeployQt",
                "PrefetchCmakeInstaller",
                "BuildAdbHelpers",
            },
            "MacPackages": {
                "ReleaseQualificationPreflight",
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchBoost",
                "BuildAdbMacX64",
                "BuildIosNativeStacks",
            },
            "MacArmPackages": {
                "ReleaseQualificationPreflight",
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchBoost",
                "BuildAdbMacArm64",
                "BuildIosNativeArm64",
            },
            "WindowsPackages": {
                "SaveVersion",
                "PrefetchCpmCache",
                "PrefetchBoost",
                "PrefetchWindowsTools",
                "BuildAdbWindowsX64",
            },
        }
        for job, expected_needs in expected_package_jobs.items():
            with self.subTest(job=job):
                self.assertEqual(needs.get(job), expected_needs)
                self.assertEqual(
                    MODULE.workflow_job_ancestors(needs, job) & mobile_jobs,
                    expected_needs & mobile_jobs,
                )

        self.assertEqual(
            needs.get("ci-gate"),
            {
                "BuildAdbLinuxArm64",
                "LinuxPackages",
                "LinuxSanitizers",
                "LinuxTsan",
                "MacPackages",
                "MacArmPackages",
                "MacSanitizers",
                "WindowsPackages",
                "WindowsX86",
                "WindowsAsan",
            },
        )

    def test_artifact_parser_resolves_anchored_platform_steps(self):
        workflow = f"""\
jobs:
  Producer:
    steps:
      - uses: actions/upload-artifact@{UPLOAD_ARTIFACT_PINNED}
        with:
          name: locked-sources
          path: sources
  Package:
    needs: Producer
    steps: &platform_steps
      - uses: actions/download-artifact@{DOWNLOAD_ARTIFACT_PINNED}
        with:
          name: locked-sources
          path: sources
  Sanitizer:
    needs: Producer
    steps: *platform_steps
"""
        self.assertEqual(
            MODULE.workflow_artifact_actions(workflow),
            {
                "Producer": {"uploads": {"locked-sources"}, "downloads": set()},
                "Package": {"uploads": set(), "downloads": {"locked-sources"}},
                "Sanitizer": {"uploads": set(), "downloads": {"locked-sources"}},
            },
        )

    def test_unknown_or_spoofed_steps_aliases_fail_closed(self):
        unknown = """\
jobs:
  Consumer:
    steps: *missing_steps
"""
        self.assertIn(
            "workflow job Consumer uses unknown steps alias missing_steps",
            MODULE.ci_build_workflow_issues(unknown),
        )

        comment_spoof = """\
jobs:
  Producer:
    # steps: &missing_steps
    steps:
      - run: true
  Consumer:
    steps: *missing_steps
"""
        self.assertIn(
            "workflow job Consumer uses unknown steps alias missing_steps",
            MODULE.ci_build_workflow_issues(comment_spoof),
        )

    def test_ci_build_rejects_a_restored_version_proxy_gate(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "  SaveVersion:\n",
            "  SaveVersion:\n    needs: [PrefetchCpmCache, PrefetchWindowsTools]\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertTrue(
            any(
                "CI root job SaveVersion must not have dependencies" in issue
                for issue in MODULE.ci_build_workflow_issues(mutated)
            )
        )

    def test_ci_build_output_references_require_direct_needs(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "  BuildIosNativeArm64:\n    needs: [SaveVersion, PrefetchIosNativeSources]",
            "  BuildIosNativeArm64:\n    needs: [PrefetchIosNativeSources]",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "CI build job BuildIosNativeArm64 references outputs from SaveVersion without a direct need",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_rejects_missing_dynamic_native_artifact_ancestry(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "PrefetchCmakeInstaller, BuildAdbHelpers",
            "PrefetchCmakeInstaller, BuildAdbHelperLegalAssets",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "CI native artifact producer BuildAdbHelpers must be an ancestor of LinuxPackages",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_rejects_a_missing_direct_artifact_dependency(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "PrefetchBoost, PrefetchOpenSsl, PrefetchWindowsTools",
            "PrefetchBoost, PrefetchOpenSsl",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        issues = MODULE.ci_build_workflow_issues(mutated)
        self.assertIn(
            "CI artifact msys2-tools producer PrefetchWindowsTools must be an ancestor of WindowsX86",
            issues,
        )

    def test_ci_build_rejects_legal_generation_in_source_prefetch(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "  BuildAdbHelperLegalAssets:\n",
            "      - run: python3 scripts/build_adb_helper_legal_assets.py\n\n"
            "  BuildAdbHelperLegalAssets:\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "ADB source prefetch must not generate version-bound legal assets",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_rejects_duplicate_ios_source_prefetch(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "      - name: Install iOS native source-build tools\n",
            "      # scripts/prefetch_ios_native_sources.py in a comment is ignored\n"
            "      - run: python3 scripts/prefetch_ios_native_sources.py --lock duplicate --output duplicate\n\n"
            "      - name: Install iOS native source-build tools\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "iOS native sources must be prefetched exactly once",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_appimage_package_row_does_not_enable_cpack_or_dummy_install_checks(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        row = workflow.split("- os: ubuntu_appimage", 1)[1].split("\n\n    runs-on:", 1)[0]
        self.assertIn("cpack_gen:\n", row)
        self.assertIn("check_container:\n", row)
        self.assertIn("check_command:\n", row)
        self.assertNotIn("NONE", row)
        self.assertNotIn("unused", row)

    def test_ci_build_rejects_unmodeled_jobs_outside_ci_gate(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow + """\
  UnmodeledPlatform:
    runs-on: ubuntu-24.04
    steps:
      - run: exit 1
"""
        self.assertTrue(
            any(
                issue.startswith(
                    "CI build workflow contains unmodeled jobs outside the reviewed gate"
                )
                for issue in MODULE.ci_build_workflow_issues(mutated)
            )
        )

    def test_ci_build_gate_must_run_after_failures(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace("    if: always()\n", "    if: success()\n", 1)
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "CI gate must run with if: always()",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_platform_jobs_must_not_continue_on_error(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "  LinuxSanitizers:\n",
            "  LinuxSanitizers:\n    continue-on-error: true\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "CI build job LinuxSanitizers must not continue on error",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_ci_build_has_no_release_publication_job(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        self.assertNotIn(
            "release publication must run outside the required CI workflow",
            MODULE.ci_build_workflow_issues(workflow),
        )

        mutated = workflow + """\
  CreatePreRelease:
    needs: [ci-gate]
    steps:
      - uses: softprops/action-gh-release@0123456789012345678901234567890123456789
"""
        self.assertIn(
            "release publication must run outside the required CI workflow",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_disabled_artifact_steps_do_not_satisfy_ownership(self):
        workflow = """\
jobs:
  Producer:
    steps:
      - uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02
        if: false
        with:
          name: locked-sources
          path: sources
  Consumer:
    steps:
      - uses: actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093
        continue-on-error: true
        with:
          name: locked-sources
          path: sources
"""
        self.assertEqual(
            MODULE.workflow_artifact_actions(workflow),
            {
                "Producer": {"uploads": set(), "downloads": set()},
                "Consumer": {"uploads": set(), "downloads": set()},
            },
        )

    def test_run_block_text_cannot_spoof_artifact_ownership(self):
        workflow = """\
jobs:
  Producer:
    steps:
      - run: |
          cat <<'YAML'
          - uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02
            with:
              name: spoofed-artifact
          YAML
"""
        self.assertEqual(
            MODULE.workflow_artifact_actions(workflow),
            {"Producer": {"uploads": set(), "downloads": set()}},
        )

    def test_ci_build_rejects_artifact_conditions_outside_supported_triggers(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        old = (
            "      - uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1\n"
            "        with:\n"
            "          name: cpm-cache\n"
        )
        new = (
            "      - uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1\n"
            "        if: ${{ github.event_name == 'schedule' }}\n"
            "        with:\n"
            "          name: cpm-cache\n"
        )
        mutated = workflow.replace(old, new, 1)
        self.assertNotEqual(mutated, workflow)
        self.assertTrue(
            any(
                "PrefetchCpmCache uploads cpm-cache must use condition None" in issue
                for issue in MODULE.ci_build_workflow_issues(mutated)
            )
        )

    def test_explicit_false_continue_on_error_remains_fail_closed(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            "  LinuxSanitizers:\n",
            "  LinuxSanitizers:\n    continue-on-error: false\n",
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertEqual(MODULE.ci_build_workflow_issues(mutated), [])

    def test_windows_package_preparation_and_artifact_upload_run_on_pull_requests(self):
        message = "Windows package preparation and artifact upload must run on pull requests"
        good = """\
jobs:
  WindowsPackages:
    strategy:
      matrix:
        config:
          - package: true
    steps:
      - uses: actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093
        if: ${{ matrix.config.package != false }}
        with:
          name: adb-helper-${{ matrix.config.adb_target }}
      - uses: ./.github/actions/agent-package-win
        if: ${{ matrix.config.package != false }}
      - name: Package tarball for upload
        if: ${{ matrix.config.package != false }}
        run: tar -czf packages-windows.tar.gz packages
      - uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02
        if: ${{ matrix.config.package != false }}
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))

        bad = good.replace(
            "if: ${{ matrix.config.package != false }}",
            "if: ${{ matrix.config.package != false && github.event_name != 'pull_request' }}",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

        spoofed = bad.replace(
            "  WindowsPackages:\n",
            "  WindowsPackages:\n    # Package preparation runs on pull_request\n",
            1,
        )
        self.assertIn(message, MODULE.ci_build_workflow_issues(spoofed))

        job_level = good.replace(
            "  WindowsPackages:\n",
            "  WindowsPackages:\n    if: ${{ github.event_name == 'push' }}\n",
            1,
        )
        self.assertIn(message, MODULE.ci_build_workflow_issues(job_level))

    def test_windows_package_composite_must_not_hide_validation_by_event(self):
        message = "Windows package composite must remain event-neutral"
        good = """\
runs:
  using: composite
  steps:
    - run: package-windows
      shell: pwsh
"""
        self.assertEqual(MODULE.windows_package_action_issues(good), [])
        bad = good.replace(
            "    - run: package-windows\n",
            "    - if: ${{ github.event_name == 'push' }}\n      run: package-windows\n",
        )
        self.assertIn(message, MODULE.windows_package_action_issues(bad))

    def test_required_ci_validation_is_decoupled_from_signing_and_notarization(self):
        message = "required CI validation must not require signing or notarization secrets"
        good = """\
jobs:
  MacValidation:
    steps:
      - uses: ./.github/actions/agent-build
      - uses: ./.github/actions/agent-run-tests
  ci-gate:
    needs: [MacValidation]
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))

        bad = good.replace(
            "      - uses: ./.github/actions/agent-run-tests\n",
            "      - uses: ./.github/actions/agent-run-tests\n"
            "      - uses: ./.github/actions/agent-package-mac\n"
            "        with:\n"
            "          codesign-identity: ${{ secrets.APPLE_DEVELOPER_ID_APPLICATION }}\n"
            "          notarization-password: ${{ secrets.NOTARIZATION_PASSWORD }}\n",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

    def test_release_secrets_are_confined_to_explicit_dispatch_qualification(self):
        message = "required CI validation must not require signing or notarization secrets"
        good = """\
on:
  workflow_dispatch:
    inputs:
      qualification-mode:
        type: choice
        options:
          - validation
          - release
jobs:
  MacPackages:
    steps:
      - uses: ./.github/actions/agent-package-mac
        with:
          qualification-mode: ${{ github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/master' && inputs.qualification-mode == 'release' && 'release' || 'validation' }}
          codesign-identity: ${{ secrets.APPLE_DEVELOPER_ID_APPLICATION }}
          notarization-password: ${{ secrets.NOTARIZATION_PASSWORD }}
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))
        bad = good.replace(
            "${{ github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/master' && inputs.qualification-mode == 'release' && 'release' || 'validation' }}",
            "validation",
            1,
        )
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

    def test_signed_ci_dispatch_master_guard_cannot_be_bypassed(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = workflow.replace(
            '          test "$GITHUB_REF" = "refs/heads/master" || {',
            '          true || test "$GITHUB_REF" = "refs/heads/master" || {',
            1,
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "signed release qualification must reject non-master dispatches",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_stable_release_master_guard_and_promotion_are_fail_closed(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-release.yml").read_text()
        self.assertEqual(MODULE.stable_release_workflow_issues(workflow), [])
        misnamed = workflow.replace(
            'name: "Publish Release (Stable)"',
            'name: "Make CI Release"',
            1,
        )
        self.assertNotEqual(misnamed, workflow)
        self.assertIn(
            'stable release workflow must be named "Publish Release (Stable)"',
            MODULE.stable_release_workflow_issues(misnamed),
        )
        bypassed = workflow.replace(
            '        test "${GITHUB_REF}" = "refs/heads/master" || {',
            '        true || test "${GITHUB_REF}" = "refs/heads/master" || {',
            1,
        )
        self.assertNotEqual(bypassed, workflow)
        self.assertIn(
            "stable release dispatch must fail closed outside master",
            MODULE.stable_release_workflow_issues(bypassed),
        )
        run_api_inputs = workflow.replace(
            '        run_event="$(gh api "$run_api" --jq \'.event\')"\n',
            '        run_event="$(gh api "$run_api" --jq \'.event\')"\n'
            '        run_qualification="$(gh api "$run_api" --jq \'.inputs["qualification-mode"] // empty\')"\n',
            1,
        )
        self.assertNotEqual(run_api_inputs, workflow)
        self.assertIn(
            "stable release evidence must come from downloaded receipts, not run API inputs",
            MODULE.stable_release_workflow_issues(run_api_inputs),
        )
        draft_tag_lookup = workflow.replace(".target_commitish", ".tag_name", 1)
        self.assertIn(
            "stable draft verification must not require an unpublished Git tag",
            MODULE.stable_release_workflow_issues(draft_tag_lookup),
        )
        without_rollback = workflow.replace(
            "        trap rollback_stable_promotion ERR\n", "", 1
        )
        self.assertIn(
            "stable release promotion must roll back every post-publish failure",
            MODULE.stable_release_workflow_issues(without_rollback),
        )

    def test_continuous_release_publish_requires_trusted_selection(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-continuous.yml").read_text()
        self.assertEqual(MODULE.continuous_release_workflow_issues(workflow), [])
        misnamed = workflow.replace(
            'name: "Publish Release (Continuous)"',
            'name: "Publish Continuous Release"',
            1,
        )
        self.assertNotEqual(misnamed, workflow)
        self.assertIn(
            'continuous release workflow must be named "Publish Release (Continuous)"',
            MODULE.continuous_release_workflow_issues(misnamed),
        )
        bypassed = workflow.replace(
            "    if: ${{ needs.select.outputs.should-publish == 'true' }}",
            "    if: always()",
            1,
        )
        self.assertNotEqual(bypassed, workflow)
        self.assertIn(
            "continuous release publication must require the verified selection output",
            MODULE.continuous_release_workflow_issues(bypassed),
        )

    def test_publication_runs_outside_the_required_ci_workflow(self):
        message = "release publication must run outside the required CI workflow"
        good = """\
jobs:
  ci-gate:
    steps:
      - run: test validation-only = validation-only
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))

        bad = good + """\
  CreatePreRelease:
    needs: [ci-gate]
    steps:
      - uses: softprops/action-gh-release@0123456789012345678901234567890123456789
"""
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

    def test_package_free_sanitizer_aggregates_do_not_need_mobile_producers(self):
        message = "package-free CI legs must not depend on mobile artifact producers"
        good = """\
jobs:
  BuildAdbHelpers:
    steps:
      - run: build-mobile-helper
  LinuxPackages:
    needs: [BuildAdbHelpers]
    strategy:
      matrix:
        config:
          - package: true
  LinuxSanitizers:
    strategy:
      matrix:
        config:
          - sanitizer: address
            package: false
  ci-gate:
    needs: [LinuxPackages, LinuxSanitizers]
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))

        bad = good.replace(
            "  LinuxSanitizers:\n",
            "  LinuxSanitizers:\n    needs: [BuildAdbHelpers]\n",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

        comment_spoof = bad.replace(
            "    needs: [BuildAdbHelpers]\n",
            "    # package-free legs do not need BuildAdbHelpers\n"
            "    needs: [BuildAdbHelpers]\n",
            1,
        )
        self.assertIn(message, MODULE.ci_build_workflow_issues(comment_spoof))

    def test_ci_build_requires_at_least_one_buildkit_cache_exporter(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        mutated = "\n".join(
            line for line in workflow.splitlines() if "cache-to:" not in line
        )
        self.assertNotEqual(mutated, workflow)
        self.assertIn(
            "BuildKit cache exports must run only on default-branch pushes",
            MODULE.ci_build_workflow_issues(mutated),
        )

    def test_buildkit_cache_export_condition_cannot_admit_pull_requests(self):
        message = "BuildKit cache exports must run only on default-branch pushes"
        good = """\
jobs:
  Linux:
    steps:
      - uses: docker/build-push-action@0123456789012345678901234567890123456789
        with:
          cache-to: ${{ github.event_name == 'push' && matrix.config.cache_write && 'type=gha,mode=min,scope=klogg-linux' || '' }}
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))
        folded = good.replace(
            "          cache-to: ${{ github.event_name == 'push' && matrix.config.cache_write && 'type=gha,mode=min,scope=klogg-linux' || '' }}",
            "          cache-to: >-\n            ${{ github.event_name == 'push' && matrix.config.cache_write && 'type=gha,mode=min,scope=klogg-linux' || '' }}",
            1,
        )
        self.assertNotEqual(folded, good)
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(folded))

        bad = good.replace(
            "github.event_name == 'push' && matrix.config.cache_write",
            "(github.event_name == 'push' || github.event_name == 'pull_request') && matrix.config.cache_write",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

    def test_ccache_keys_are_bounded_and_do_not_use_run_ids(self):
        message = "ccache keys must be bounded and must not use github.run_id"
        good = """\
jobs:
  Linux:
    steps:
      - uses: actions/cache/save@caa296126883cff596d87d8935842f9db880ef25
        with:
          path: ${{ github.workspace }}/.ccache
          key: linux-ccache-v2-${{ hashFiles('CMakeLists.txt', '3rdparty/CMakeLists.txt') }}
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))

        bad = good.replace(
            "${{ hashFiles('CMakeLists.txt', '3rdparty/CMakeLists.txt') }}",
            "${{ github.run_id }}",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

        comment_spoof = bad.replace(
            "          key:",
            "          # bounded key: hashFiles('CMakeLists.txt')\n          key:",
            1,
        )
        self.assertIn(message, MODULE.ci_build_workflow_issues(comment_spoof))

    def test_ccache_refreshes_daily_without_unbounded_per_run_keys(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        self.assertEqual(workflow.count("current={today.isoformat()}"), 2)
        self.assertEqual(workflow.count("days=1"), 2)
        self.assertNotIn("isocalendar()", workflow)

    def test_ccache_is_saved_only_after_tests_and_package_qualification(self):
        workflow = (ROOT / ".github" / "workflows" / "ci-build.yml").read_text()
        linux = workflow.split("  LinuxPackages:", 1)[1].split("\n  LinuxSanitizers:", 1)[0]
        self.assertGreater(
            linux.index("- name: Save ccache for the Linux build"),
            linux.index("- uses: ./.github/actions/docker-run-tests"),
        )
        mac = workflow.split("  MacPackages:", 1)[1].split("\n  MacArmPackages:", 1)[0]
        self.assertGreater(
            mac.index("- name: Save ccache for the macOS build"),
            mac.index("- uses: ./.github/actions/agent-package-mac"),
        )

    def test_ccache_entries_are_capped_to_the_repository_budget(self):
        message = "ccache entries must be capped at 250M"
        good = """\
jobs:
  Linux:
    steps:
      - run: ccache --max-size=250M && ccache --cleanup
      - uses: actions/cache/save@55cc8345863c7cc4c66a329aec7e433d2d1c52a9
        with:
          path: ${{ github.workspace }}/.ccache
          key: linux-ccache-v2-week
"""
        self.assertNotIn(message, MODULE.ci_build_workflow_issues(good))
        bad = good.replace("--max-size=250M", "--max-size=500M")
        self.assertIn(message, MODULE.ci_build_workflow_issues(bad))

    def test_codeql_requires_pinned_matching_actions_and_timeout(self):
        insecure = """\
jobs:
  analyze:
    runs-on: ubuntu-24.04
    steps:
      - uses: github/codeql-action/init@v3
      - uses: github/codeql-action/analyze@v3
"""
        issues = MODULE.codeql_workflow_issues(insecure)
        self.assertTrue(any("init must use" in issue for issue in issues))
        self.assertTrue(any("analyze must use" in issue for issue in issues))
        self.assertTrue(any("timeout-minutes" in issue for issue in issues))

    def test_codeql_rejects_mismatched_action_revisions(self):
        text = secure_codeql_workflow().replace(
            f"github/codeql-action/analyze@{CODEQL_PINNED}",
            f"github/codeql-action/analyze@{'a' * 40}",
            1,
        )
        issues = MODULE.codeql_workflow_issues(text)
        self.assertEqual(len(issues), 1)
        self.assertIn("same reviewed SHA", issues[0])

    def test_codeql_rejects_continue_on_error(self):
        text = secure_codeql_workflow().replace(
            "    timeout-minutes: 30\n",
            "    timeout-minutes: 30\n    continue-on-error: true\n",
            1,
        )
        issues = MODULE.codeql_workflow_issues(text)
        self.assertEqual(len(issues), 1)
        self.assertIn("continue-on-error", issues[0])

    def test_secure_codeql_workflow_is_accepted(self):
        self.assertEqual(MODULE.codeql_workflow_issues(secure_codeql_workflow()), [])

    def test_codeql_runs_on_master_pushes(self):
        message = "CodeQL workflow must run on pushes to master"
        good = secure_codeql_workflow()
        self.assertNotIn(message, MODULE.codeql_workflow_issues(good))

        bad = good.replace("  push:\n    branches: [master]\n", "", 1)
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.codeql_workflow_issues(bad))

        comment_spoof = bad.replace(
            "on:\n",
            "on:\n  # push:\n  #   branches: [master]\n",
            1,
        )
        self.assertIn(message, MODULE.codeql_workflow_issues(comment_spoof))

    def test_codeql_init_uses_explicit_manual_build_mode(self):
        message = "CodeQL init must set build-mode: manual"
        good = secure_codeql_workflow()
        self.assertNotIn(message, MODULE.codeql_workflow_issues(good))

        missing = good.replace("          build-mode: manual\n", "", 1)
        self.assertNotEqual(missing, good)
        self.assertIn(message, MODULE.codeql_workflow_issues(missing))

        no_build = good.replace("          build-mode: manual", "          build-mode: none", 1)
        self.assertNotEqual(no_build, good)
        self.assertIn(message, MODULE.codeql_workflow_issues(no_build))

        comment_spoof = missing.replace(
            "          languages: c-cpp\n",
            "          languages: c-cpp\n          # build-mode: manual\n",
            1,
        )
        self.assertIn(message, MODULE.codeql_workflow_issues(comment_spoof))

    def test_codeql_runs_on_pull_requests_and_manual_dispatch(self):
        good = secure_codeql_workflow()
        for trigger, marker in (
            ("pull_request", "CodeQL workflow must run on pull requests to master"),
            ("workflow_dispatch", "CodeQL workflow must support manual dispatch"),
        ):
            with self.subTest(trigger=trigger):
                self.assertNotIn(marker, MODULE.codeql_workflow_issues(good))

        without_pull_request = good.replace(
            "  pull_request:\n    branches: [master]\n", "", 1
        )
        self.assertIn(
            "CodeQL workflow must run on pull requests to master",
            MODULE.codeql_workflow_issues(without_pull_request),
        )

        without_dispatch = good.replace("  workflow_dispatch:\n", "", 1)
        self.assertIn(
            "CodeQL workflow must support manual dispatch",
            MODULE.codeql_workflow_issues(without_dispatch),
        )

    def test_codeql_security_permissions_are_fail_closed_and_job_scoped(self):
        message = (
            "CodeQL analyze job must grant only contents: read and "
            "security-events: write permissions"
        )
        good = secure_codeql_workflow()
        self.assertNotIn(message, MODULE.codeql_workflow_issues(good))

        missing_security_events = good.replace("      security-events: write\n", "", 1)
        self.assertIn(message, MODULE.codeql_workflow_issues(missing_security_events))

        read_only_security_events = good.replace(
            "      security-events: write", "      security-events: read", 1
        )
        self.assertIn(message, MODULE.codeql_workflow_issues(read_only_security_events))

        overprivileged = good.replace(
            "      contents: read\n",
            "      contents: read\n      actions: read\n",
            1,
        )
        self.assertIn(message, MODULE.codeql_workflow_issues(overprivileged))

        workflow_scoped = good.replace(
            "jobs:\n  analyze:\n    permissions:\n"
            "      contents: read\n"
            "      security-events: write\n",
            "permissions:\n"
            "  contents: read\n"
            "  security-events: write\n"
            "jobs:\n"
            "  analyze:\n",
            1,
        )
        self.assertNotEqual(workflow_scoped, good)
        self.assertIn(message, MODULE.codeql_workflow_issues(workflow_scoped))

    def test_codeql_manual_build_disables_overlay_analysis(self):
        message = "CodeQL traced build must explicitly disable overlay analysis"
        good = secure_codeql_workflow()
        self.assertNotIn(message, MODULE.codeql_workflow_issues(good))
        for marker in (
            '      CODEQL_ACTION_OVERLAY_ANALYSIS: "false"\n',
            '      CODEQL_ACTION_OVERLAY_ANALYSIS_CODE_SCANNING_CPP: "false"\n',
        ):
            with self.subTest(marker=marker):
                bad = good.replace(marker, "", 1)
                self.assertNotEqual(bad, good)
                self.assertIn(message, MODULE.codeql_workflow_issues(bad))

    def test_codeql_manual_build_restores_dependencies_and_builds_application(self):
        good = secure_codeql_workflow()
        mutations = (
            (
                "      - uses: ./.github/actions/agent-setup\n",
                "CodeQL manual build must restore the shared dependency closure",
            ),
            (
                "      - run: cmake --build build -t klogg\n",
                "CodeQL manual build must trace the klogg application target",
            ),
        )
        for marker, message in mutations:
            with self.subTest(marker=marker):
                bad = good.replace(marker, "", 1)
                self.assertNotEqual(bad, good)
                self.assertIn(message, MODULE.codeql_workflow_issues(bad))

    def test_codeql_build_cannot_be_spoofed_by_an_unrelated_job(self):
        good = secure_codeql_workflow()
        bad = good.replace(
            "      - run: cmake --build build -t klogg\n", "", 1
        ) + """\
  Unrelated:
    steps:
      - run: cmake --build build -t klogg
"""
        self.assertIn(
            "CodeQL manual build must trace the klogg application target",
            MODULE.codeql_workflow_issues(bad),
        )

    def test_codeql_uses_the_current_action_generation(self):
        message = "CodeQL actions must use the current reviewed generation"
        good = secure_codeql_workflow()
        self.assertNotIn(message, MODULE.codeql_workflow_issues(good))

        bad = good.replace(CODEQL_PINNED, NODE20_CODEQL_PINNED)
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.codeql_workflow_issues(bad))

        comment_spoof = bad.replace("# v4.37.9", "# current v4 generation")
        self.assertIn(message, MODULE.codeql_workflow_issues(comment_spoof))

    def test_repository_codeql_workflow_uses_warning_free_traced_build(self):
        workflow = (
            ROOT / ".github" / "workflows" / "codeql-analysis.yml"
        ).read_text()
        self.assertEqual(MODULE.codeql_workflow_issues(workflow), [])

        overlay_enabled = workflow.replace(
            '      CODEQL_ACTION_OVERLAY_ANALYSIS_CODE_SCANNING_CPP: "false"',
            '      CODEQL_ACTION_OVERLAY_ANALYSIS_CODE_SCANNING_CPP: "true"',
            1,
        )
        self.assertNotEqual(overlay_enabled, workflow)
        self.assertIn(
            "CodeQL traced build must explicitly disable overlay analysis",
            MODULE.codeql_workflow_issues(overlay_enabled),
        )

    def test_agent_setup_self_populates_cold_cpm_caches(self):
        action = (
            ROOT / ".github" / "actions" / "agent-setup" / "action.yml"
        ).read_text()
        self.assertEqual(MODULE.agent_setup_cpm_issues(action), [])

        without_prefetch = action.replace(
            "      uses: ./.github/actions/prefetch-cpm-cache\n", "", 1
        )
        self.assertNotEqual(without_prefetch, action)
        self.assertIn(
            "agent setup must populate the CPM source cache after restore",
            MODULE.agent_setup_cpm_issues(without_prefetch),
        )

    def test_scans_yaml_workflows_and_composite_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workflow = root / ".github/workflows/test.yaml"
            action_yml = root / ".github/actions/one/action.yml"
            action_yaml = root / ".github/actions/two/action.yaml"
            for path in (workflow, action_yml, action_yaml):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("steps:\n  - uses: actions/checkout@v4\n")
            self.assertEqual(
                {path.relative_to(root) for path in MODULE.ci_manifests(root)},
                {
                    pathlib.Path(".github/workflows/test.yaml"),
                    pathlib.Path(".github/actions/one/action.yml"),
                    pathlib.Path(".github/actions/two/action.yaml"),
                },
            )

    def test_broad_sanitizer_patterns_are_rejected(self):
        self.assertIsNotNone(MODULE.BROAD_SUPPRESSION_RE.search("race:*Qt*\n"))
        self.assertIsNotNone(MODULE.BROAD_SUPPRESSION_RE.search("leak:*mimalloc*\n"))
        self.assertIsNone(MODULE.BROAD_SUPPRESSION_RE.search("leak:qoffscreen\n"))

    def test_macos_lsan_rule_ignores_explanatory_comments(self):
        self.assertFalse(
            MODULE.has_unsupported_macos_lsan(
                "# detect_leaks=1 is unsupported on Apple ASan\n"
                'echo "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1"\n'
            )
        )
        self.assertTrue(
            MODULE.has_unsupported_macos_lsan(
                'echo "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1"\n'
            )
        )

    def test_coverage_floor_rejects_non_finite_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "floor.txt"
            for value in ("NaN", "inf", "-inf", "0", "101"):
                with self.subTest(value=value):
                    path.write_text(value)
                    with self.assertRaises(ValueError):
                        MODULE.coverage_floor(path)

    def test_coverage_workflow_requires_root_sources_and_production_targets(self):
        insecure = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests
      --filter '.*/src/.*'
      --filter '.*/src/.*'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(insecure)), 6)

        secure = """\
env:
  EVENT_NAME: ${{ github.event_name }}
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
      git fetch --no-tags origin "$base_sha"
      git cat-file -e "$base_sha^{commit}"
      echo "coverage ratchet base commit"
"""
        self.assertEqual(MODULE.coverage_workflow_issues(secure), [])

    def test_configured_test_targets_join_the_coverage_aggregate(self):
        options = (ROOT / "cmake" / "TestTargetOptions.cmake").read_text()
        self.assertIn("add_dependencies(klogg_test_build ${target})", options)

    def test_coverage_ratchet_requires_the_authoritative_event_base(self):
        workflow = (
            ROOT / ".github" / "workflows" / "coverage.yml"
        ).read_text()
        self.assertNotIn(
            "coverage ratchet must fail closed on the event base commit",
            MODULE.coverage_workflow_issues(workflow),
        )

        insecure = workflow.replace(
            'git fetch --no-tags origin "$base_sha"',
            'base_sha=$(git rev-parse HEAD^ 2>/dev/null || true)',
            1,
        )
        self.assertIn(
            "coverage ratchet must fail closed on the event base commit",
            MODULE.coverage_workflow_issues(insecure),
        )

    def test_coverage_comments_cannot_spoof_scope_or_targets(self):
        workflow = """\
steps:
  # cmake --build build_root -t klogg klogg_grep klogg_test_build
  - run: |
      # --filter '^src/'
      # --filter '^src/'
      cmake --build build_root -t klogg_tests klogg_itests
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 6)

    def test_inline_shell_comments_cannot_spoof_coverage_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests : # klogg klogg_grep
      true # --filter '^src/'
      true # --filter '^src/'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 6)

    def test_coverage_noop_tokens_cannot_spoof_report_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      echo "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file"
      echo "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file"
      gcovr --filter '^src/' --html
      gcovr --filter '^src/' --json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_every_gcovr_command_requires_the_narrow_workaround(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file
      gcovr --filter '^src/' --json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_coverage_reports_must_fail_closed(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html || echo ignored
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json || :
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn("coverage report generation must fail closed", issues)

    def test_coverage_requires_one_html_and_one_json_report(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index-2.html
"""
        self.assertIn(
            "coverage must produce the authoritative HTML summary and JSON report",
            MODULE.coverage_workflow_issues(workflow),
        )

    def test_coverage_targets_cannot_be_spoofed_after_shell_operators(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests && printf '%s' klogg klogg_grep
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn("coverage build must include target klogg", issues)
        self.assertIn("coverage build must include target klogg_grep", issues)

    def test_coverage_options_cannot_be_spoofed_inside_another_argument(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --html-title "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file" --html-details --print-summary -o coverage_report/index.html
      gcovr --html-title "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file" --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "gcovr must include root-relative src/ paths in every report pass",
            issues,
        )
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_coverage_and_lists_cannot_mask_report_failure(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html && :
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        self.assertIn(
            "coverage report generation must fail closed",
            MODULE.coverage_workflow_issues(workflow),
        )

    def test_coverage_rejects_duplicate_scope_and_output_options(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_test_build
      gcovr --filter '^src/' --filter '.*' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html -o /tmp/redirected.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "gcovr must include root-relative src/ paths in every report pass",
            issues,
        )
        self.assertIn(
            "coverage must produce the authoritative HTML summary and JSON report",
            issues,
        )

    def test_clang_tidy_diff_find_fallback_preserves_explicit_error(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn(
            "find /usr/lib/llvm-*/share/clang -name clang-tidy-diff.py -print -quit 2>/dev/null || true",
            workflow,
        )

    def test_static_analysis_installs_optional_dependency_build_tools(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("libcurl4-openssl-dev", workflow)
        self.assertIn("ragel", workflow)

    def test_static_analysis_full_audit_and_changed_events_do_not_cancel_each_other(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("group: ${{ github.workflow }}-${{ github.event_name }}-${{ github.event_name == 'workflow_dispatch' && github.run_id || github.ref }}", workflow)
        self.assertIn("cancel-in-progress: ${{ github.event_name != 'schedule' }}", workflow)

    def test_static_analysis_dispatch_is_full_by_default_or_uses_an_explicit_base(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("analysis-mode:", workflow)
        self.assertIn("default: full-report", workflow)
        self.assertIn("MANUAL_BASE_SHA: ${{ inputs.base-sha }}", workflow)
        self.assertNotIn('workflow_dispatch) base_sha="$(git rev-parse HEAD^)"', workflow)
        self.assertIn('git fetch --no-tags origin "$base_sha"', workflow)

    def test_static_analysis_uses_reusable_changed_line_runner(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("python3 scripts/run_changed_clang_tidy.py", workflow)
        self.assertIn('--base "$KLOGG_ANALYSIS_BASE"', workflow)
        self.assertIn("PUSH_BASE_SHA: ${{ github.event.before }}", workflow)
        self.assertIn('--build-dir "$KLOGG_BUILD_ROOT"', workflow)
        self.assertIn('--clang-tidy-diff "$CLANG_TIDY_DIFF"', workflow)

    def test_static_analysis_is_equally_strict_on_pull_requests_and_master_pushes(self):
        message = "clang-tidy findings must fail both pull requests and master pushes"
        good = """\
concurrency:
  group: ${{ github.workflow }}-${{ github.event_name }}-${{ github.event_name == 'workflow_dispatch' && github.run_id || github.ref }}
  cancel-in-progress: ${{ github.event_name != 'schedule' }}
env:
  PR_BASE_SHA: ${{ github.event.pull_request.base.sha }}
  PUSH_BASE_SHA: ${{ github.event.before }}
  MANUAL_BASE_SHA: ${{ inputs.base-sha }}
steps:
  - name: Run clang-tidy
    run: |
      if [ "$KLOGG_ANALYSIS_MODE" = "full-report" ]; then
        xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
      else
        case "$EVENT_NAME" in
          pull_request) base_sha="$PR_BASE_SHA" ;;
          push) base_sha="$PUSH_BASE_SHA" ;;
          workflow_dispatch) base_sha="$MANUAL_BASE_SHA" ;;
        esac
        git fetch --no-tags origin "$base_sha"
        python3 scripts/run_changed_clang_tidy.py --base "$KLOGG_ANALYSIS_BASE" --build-dir "$KLOGG_BUILD_ROOT" --clang-tidy-diff "$CLANG_TIDY_DIFF"
      fi
"""
        issues = MODULE.static_analysis_workflow_issues(good)
        self.assertNotIn(message, issues)
        consumer_message = (
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers"
        )
        self.assertNotIn(consumer_message, issues)

        without_report_only_consumer = good.replace(
            "        xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\"' _ < tidy_files.nul\n",
            "        true\n",
            1,
        )
        self.assertNotEqual(without_report_only_consumer, good)
        self.assertIn(
            consumer_message,
            MODULE.static_analysis_workflow_issues(without_report_only_consumer),
        )

        bad = good.replace(
            "  cancel-in-progress: ${{ github.event_name != 'schedule' }}",
            "  cancel-in-progress: true",
            1,
        )
        self.assertNotEqual(bad, good)
        self.assertIn(message, MODULE.static_analysis_workflow_issues(bad))

    def test_static_analysis_rejects_masked_report_only_tool_failures(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        mutated = workflow.replace(
            "' _ < tidy_files.nul\n",
            "' _ < tidy_files.nul || true\n",
            1,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(mutated),
        )

    def test_static_analysis_rejects_duplicate_runner_options(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        mutated = workflow.replace(
            "--jobs \"$(nproc)\"",
            "--jobs \"$(nproc)\" --base HEAD",
            1,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(mutated),
        )

    def test_static_analysis_uses_only_configured_first_party_units(self):
        insecure = """\
steps:
  - run: |
      find src -type f -name '*.cpp' > tidy_files.txt
      cmake -DKLOGG_USE_SENTRY=OFF
"""
        issues = MODULE.static_analysis_workflow_issues(insecure)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            issues,
        )

        secure = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -DCPM_SOURCE_CACHE="$KLOGG_WORKSPACE/cpm_cache" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertEqual(MODULE.static_analysis_workflow_issues(secure), [])

    def test_static_analysis_configures_vectorscan_production_code(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        self.assertIn("-DKLOGG_USE_VECTORSCAN=ON", workflow)
        self.assertNotIn("-DKLOGG_USE_VECTORSCAN=OFF", workflow)

        insecure = workflow.replace(
            "-DKLOGG_USE_VECTORSCAN=ON",
            "-DKLOGG_USE_VECTORSCAN=OFF",
            1,
        )
        self.assertIn(
            "static analysis must configure optional Vectorscan production code",
            MODULE.static_analysis_workflow_issues(insecure),
        )

    def test_static_analysis_configure_requires_the_prefetched_cpm_cache(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        message = (
            "static analysis configure must use the prefetched CPM cache "
            "fully disconnected"
        )
        self.assertNotIn(message, MODULE.static_analysis_workflow_issues(workflow))

        without_cache = workflow.replace(
            '            -DCPM_SOURCE_CACHE="$KLOGG_WORKSPACE/cpm_cache" \\\n',
            "",
            1,
        )
        self.assertNotEqual(without_cache, workflow)
        self.assertIn(message, MODULE.static_analysis_workflow_issues(without_cache))

    def test_static_analysis_cppcheck_scope_resolves_relative_compile_entries(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        message = "cppcheck TU scope must resolve relative compile database entries"
        self.assertNotIn(message, MODULE.static_analysis_workflow_issues(workflow))

        without_filter = workflow.replace(
            "          python3 scripts/filter_compile_database.py \\\n"
            "            \"$KLOGG_BUILD_ROOT/compile_commands.json\" \"$KLOGG_WORKSPACE\" \\\n"
            "            /tmp/cppcheck_files.nul /tmp/cc_src.json\n",
            "          cp \"$KLOGG_BUILD_ROOT/compile_commands.json\" /tmp/cc_src.json\n",
            1,
        )
        self.assertNotEqual(without_filter, workflow)
        self.assertIn(message, MODULE.static_analysis_workflow_issues(without_filter))

    def test_static_analysis_changed_path_discovery_excludes_deleted_files(self):
        # A PR that deletes a src/ file must not feed the deleted path to
        # clang-tidy/cppcheck; both gates discover changed paths with
        # `git diff --name-only -z` and must pass --diff-filter=d.
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        message = "changed-path discovery must exclude deleted files (--diff-filter=d)"
        self.assertNotIn(message, MODULE.static_analysis_workflow_issues(workflow))

        insecure = workflow.replace(" --diff-filter=d", "", 1)
        self.assertNotEqual(insecure, workflow)
        self.assertIn(message, MODULE.static_analysis_workflow_issues(insecure))

    def test_static_analysis_sentry_flag_must_belong_to_the_real_configure(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT"
      cmake -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -P verify_sentry.cmake
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_requires_nul_consumers_for_the_tu_list(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -n 1 clang-tidy < tidy_files.nul
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            issues,
        )

    def test_static_analysis_noop_tokens_and_overrides_are_rejected(self):
        workflow = """\
steps:
  - run: |
      echo "python3 scripts/first_party_compile_units.py db src > tidy_files.txt"
      find src -type f -name '*.cpp' > tidy_files.txt
      cmake -S . -B build -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -DKLOGG_USE_SENTRY=OFF
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            issues,
        )

    def test_static_analysis_rejects_wrong_scope_and_fake_consumers(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py wrong.json src/ui --null > tidy_files.nul
      xargs -0 -n 1 echo clang-tidy < tidy_files.nul
      xargs -0 -n 1 bash -c 'echo clang-tidy "$1" || exit 0' _ < tidy_files.nul || true
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            issues,
        )

    def test_static_analysis_consumers_must_forward_each_tu_to_clang_tidy(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" src/benign.cpp' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" src/benign.cpp || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_consumers_reject_xargs_batch_and_fixed_arguments(self):
        consumers = (
            (
                "xargs -0 -n 2 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 2 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ src/benign.cpp < tidy_files.nul",
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ src/benign.cpp < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 true bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 1 true bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 -a benign.nul bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 1 -a benign.nul bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul < benign.nul",
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul < benign.nul || true",
            ),
        )
        for strict_consumer, report_consumer in consumers:
            with self.subTest(strict_consumer=strict_consumer):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      {strict_consumer}
      {report_consumer}
"""
                self.assertIn(
                    "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_requires_the_strict_pr_consumer(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_strict_consumer_must_propagate_exit_status(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"; exit 0' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_later_sentry_reconfigure_override(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      cmake -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=OFF
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_typed_and_unset_sentry_reconfiguration(self):
        reconfigures = (
            'cmake -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B "$KLOGG_BUILD_ROOT" -U KLOGG_USE_SENTRY',
            'cmake -B "$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B"$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B"$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B="$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B="$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B "$KLOGG_BUILD_ROOT"',
            'cmake -B "$KLOGG_BUILD_ROOT" --fresh',
        )
        for reconfigure in reconfigures:
            with self.subTest(reconfigure=reconfigure):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      {reconfigure}
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
                self.assertIn(
                    "static analysis must configure optional Sentry production code",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_rejects_all_false_sentry_spellings(self):
        for false_value in ("OFF", "FALSE", "NO", "0"):
            with self.subTest(false_value=false_value):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -DKLOGG_USE_SENTRY={false_value}
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
                self.assertIn(
                    "static analysis must configure optional Sentry production code",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_uses_the_shared_complete_cpm_cache_key(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertNotIn("KLOGG_CPM_CACHE_KEY_SUFFIX", workflow)

    def test_cppcheck_baselines_require_path_and_line(self):
        self.assertEqual(
            len(MODULE.cppcheck_suppression_issues("unreadVariable\n")), 1
        )
        self.assertEqual(
            MODULE.cppcheck_suppression_issues(
                "unreadVariable:*/src/example.cpp:42\n"
            ),
            [],
        )


if __name__ == "__main__":
    unittest.main()
