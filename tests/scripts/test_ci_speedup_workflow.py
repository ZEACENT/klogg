"""Latency changes must preserve final native-artifact and publication gates."""
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from test_ci_environment_workflow import QUALITY

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/ci-build.yml'


def steps(text, job):
    return [QUALITY.workflow_step_fields(step)
            for step in QUALITY.workflow_job_steps(text)[job]]


class EarlyMacCompilationTest(unittest.TestCase):
    def assert_overlap(self, text, job):
        block = QUALITY.workflow_job_blocks(text)[job]
        needs = QUALITY.workflow_job_direct_value(block, 'needs')
        self.assertNotIn('BuildAdb', needs)
        self.assertNotIn('BuildIosNative', needs)
        rows = steps(text, job)
        prepare = next(i for i, (fields, children) in enumerate(rows)
                       if fields.get('uses') == './.github/actions/agent-build'
                       and children.get('with', {}).get('phase') == 'prepare')
        rendezvous = next(i for i, (fields, _) in enumerate(rows)
                          if fields.get('id') == 'native-ready')
        final = next(i for i, (fields, children) in enumerate(rows)
                     if fields.get('uses') == './.github/actions/agent-build'
                     and 'finalize' in children.get('with', {}).get('phase', ''))
        tests = next(i for i, (fields, _) in enumerate(rows)
                     if fields.get('uses') == './.github/actions/agent-run-tests')
        package = next(i for i, (fields, _) in enumerate(rows)
                       if fields.get('uses') == './.github/actions/agent-package-mac')
        self.assertLess(prepare, rendezvous)
        self.assertLess(rendezvous, final)
        self.assertLess(final, tests)
        self.assertLess(tests, package)
        for index in (prepare, rendezvous):
            self.assertEqual(rows[index][0]['if'], "${{ env.KLOGG_PACKAGE_ENABLED == 'true' }}")
        self.assertIn('scripts/ci_native_rendezvous.py', rows[rendezvous][0]['run'])
        self.assertEqual(rows[rendezvous][1]['env']['GH_TOKEN'], '${{ github.token }}')
        self.assertEqual(rows[final][1]['with']['native-receipt'], '${{ runner.temp }}/native-ready.json')
        native_ids = {'adb-artifact-id', 'support-artifact-id', 'ios-artifact-id'}
        found = set()
        for i, (fields, children) in enumerate(rows):
            options = children.get('with', {})
            identifier = options.get('artifact-ids', '')
            for name in native_ids:
                if identifier == '${{ steps.native-ready.outputs.' + name + ' }}':
                    self.assertNotIn('name', options)
                    self.assertLess(rendezvous, i)
                    self.assertLess(i, final)
                    found.add(name)
            if fields.get('name', '').startswith('Verify target-bound'):
                self.assertLess(rendezvous, i)
                self.assertLess(i, final)
            if fields.get('uses') in ('./.github/actions/save-verified-compiler-cache',
                                      './.github/actions/export-compiler-object-artifact',
                                      './.github/actions/upload-release-artifact'):
                self.assertLess(package, i, 'preliminary objects must never be published')
        self.assertEqual(found, native_ids)

    def test_lint_accepts_runtime_dependencies_only_with_all_overlap_guards(self):
        text = WORKFLOW.read_text()
        self.assertEqual(QUALITY.ci_build_workflow_issues(text), [])
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['MacPackages'])
        changes = (
            ('--architecture "$KLOGG_IOS_ARCHITECTURE"', '--architecture arm64'),
            ('artifact-ids: ${{ steps.native-ready.outputs.adb-artifact-id }}', 'artifact-ids: 123'),
            ('merge-multiple: true', 'merge-multiple: false'),
            ('phase: prepare', 'phase: full'),
            ('native-receipt: ${{ runner.temp }}/native-ready.json', 'native-receipt: /tmp/stale.json'),
            ('id: native-ready', 'id: native-ready\n        continue-on-error: true'),
        )
        for before, after in changes:
            with self.subTest(change=before):
                self.assertIn(before, block)
                changed = text.replace(block, block.replace(before, after, 1), 1)
                self.assertTrue(QUALITY.ci_build_workflow_issues(changed))

    def test_package_jobs_compile_before_waiting_and_still_finalize_and_verify(self):
        for job in ('MacPackages', 'MacArmPackages'):
            with self.subTest(job=job):
                self.assert_overlap(WORKFLOW.read_text(), job)


class EarlyWindowsCompilationTest(unittest.TestCase):
    def test_windows_release_overlaps_and_requires_authenticated_finalization(self):
        text = WORKFLOW.read_text()
        needs = QUALITY.workflow_job_needs(text)
        self.assertNotIn('BuildAdbWindowsX64', needs['WindowsPackages'])
        self.assertEqual(QUALITY.native_overlap_workflow_issues(text, 'WindowsPackages', platform='Windows'), [])
        self.assertEqual(QUALITY.ci_build_workflow_issues(text), [])
        rows = steps(text, 'WindowsPackages')
        names = [fields.get('name') for fields, _ in rows]
        guard = names.index('Verify Windows native overlap')
        prepare = names.index('Compile Windows objects while native producer runs')
        self.assertLess(guard, prepare)
        self.assertLess(names.index('Prepare dev cmd'), guard)
        for cache_step in ('Prepare verified Windows object cache', 'Activate Windows object cache or use the native compiler'):
            self.assertLess(guard, names.index(cache_step), 'fixture builds must precede the real cache session')
        self.assertEqual(rows[guard][0]['shell'], 'pwsh')
        self.assertNotIn('if', rows[guard][0])
        self.assertNotIn('continue-on-error', rows[guard][0])
        self.assertEqual(rows[guard][0]['run'].strip(),
                         'python -I tests/scripts/test_ci_native_overlap.py WindowsNativeOverlapTests\nexit $LASTEXITCODE')
        descriptor = next(fields['run'] for fields, _ in rows if fields.get('id') == 'shared-cache-descriptor')
        self.assertIn('--required-step "Compile Windows objects while native producer runs"', descriptor)
        metrics = next(children['with']['path'] for fields, children in rows
                       if fields.get('name') == 'Upload current-run build metrics')
        self.assertEqual(metrics, '${{ env.KLOGG_BUILD_ROOT }}/ci-build-metrics-${{ github.run_id }}-${{ github.run_attempt }}.json')
        evidence = next(children['with']['path'] for fields, children in rows
                        if fields.get('name') == 'Upload Windows phased-build evidence')
        self.assertIn('ci-build-phase-${{ github.run_id }}-${{ github.run_attempt }}-*.ninja-log.txt', evidence)

    @staticmethod
    def selected_upload_files(root, specification):
        replacements = {'${{ env.KLOGG_BUILD_ROOT }}': 'build', '${{ github.run_id }}': '17',
                        '${{ github.run_attempt }}': '2'}
        selected = set()
        for pattern in specification['path'].splitlines():
            for key, value in replacements.items():
                pattern = pattern.replace(key, value)
            if '${{' in pattern:
                raise AssertionError('unmodeled upload path: ' + pattern)
            selected.update(path for path in root.glob(pattern.strip()) if path.is_file())
        return sorted(selected)

    def test_metrics_upload_payload_remains_readable_by_existing_collector(self):
        spec = importlib.util.spec_from_file_location('metrics_consumer', ROOT / 'scripts/ci_perf_report.py')
        consumer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(consumer)
        value = {'build_wall_ms': 123, 'provenance': {'run_id': '17', 'run_attempt': '2'}}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            build.mkdir()
            aggregate = build / 'ci-build-metrics-17-2.json'
            aggregate.write_text(json.dumps(value))
            for phase in ('prepare', 'finalize'):
                (build / ('ci-build-phase-17-2-' + phase + '.json')).write_text(json.dumps({'phase': phase}))
                (build / ('ci-build-phase-17-2-' + phase + '.ninja-log.txt')).write_text('# ninja log v5\n')
            for job in ('WindowsPackages', 'WindowsX86', 'WindowsAsan', 'MacPackages', 'MacArmPackages'):
                with self.subTest(job=job):
                    upload = next(children['with'] for fields, children in steps(WORKFLOW.read_text(), job)
                                  if fields.get('name') == 'Upload current-run build metrics')
                    payload = io.BytesIO()
                    with zipfile.ZipFile(payload, 'w') as archive:
                        for path in self.selected_upload_files(root, upload):
                            archive.writestr(path.name, path.read_bytes())
                    self.assertEqual(consumer.read_metrics_zip(payload.getvalue(), 17, 2), value)
                    self.assertEqual(self.selected_upload_files(root, upload), [aggregate])

    def test_raw_only_failed_phase_uploads_separate_evidence_not_metrics(self):
        rows = steps(WORKFLOW.read_text(), 'WindowsPackages')
        phase_steps = [(fields, children['with']) for fields, children in rows
                       if fields.get('name') == 'Upload Windows phased-build evidence']
        self.assertEqual(len(phase_steps), 1)
        fields, evidence = phase_steps[0]
        self.assertTrue(evidence['name'].startswith('build-phase-evidence-'))
        self.assertEqual(fields['if'], "${{ always() && env.KLOGG_PACKAGE_ENABLED == 'true' && hashFiles(format('{0}/ci-build-phase-{1}-{2}-*', env.KLOGG_BUILD_ROOT, github.run_id, github.run_attempt)) != '' }}")
        metrics = next(children['with'] for item, children in rows
                       if item.get('name') == 'Upload current-run build metrics')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'build').mkdir()
            raw = root / 'build/ci-build-phase-17-2-prepare.ninja-log.txt'
            raw.write_text('# ninja log v5\n0\t1\t0\tpartial.obj\t123\n')
            self.assertEqual(self.selected_upload_files(root, evidence), [raw])
            self.assertEqual(self.selected_upload_files(root, metrics), [])
            pattern = re.search(r"hashFiles\(format\('([^']+)'", fields['if']).group(1).format('build', 17, 2)
            self.assertEqual(sorted(root.glob(pattern)), [raw], 'raw-only failure evidence must trigger the upload')

    def test_native_verifier_custom_nonexecuting_shell_is_rejected(self):
        text = WORKFLOW.read_text()
        verifier = next(step for step in QUALITY.workflow_job_steps(text)['WindowsPackages']
                        if QUALITY.workflow_step_fields(step)[0].get('name') ==
                        'Verify target-bound ADB helper checksum envelope and provenance')
        original = '\n'.join(verifier)
        self.assertIn('shell: bash', original)
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['WindowsPackages'])
        self.assertIn(original, block)
        mutated = text.replace(block, block.replace(original, original.replace('shell: bash', 'shell: echo {0}', 1), 1), 1)
        changed = next(fields for fields, _ in steps(mutated, 'WindowsPackages')
                       if fields.get('name') == 'Verify target-bound ADB helper checksum envelope and provenance')
        self.assertEqual(changed['shell'], 'echo {0}')
        for checker in (lambda value: QUALITY.native_overlap_workflow_issues(value, 'WindowsPackages', platform='Windows'),
                        QUALITY.ci_build_workflow_issues):
            with self.subTest(checker=checker.__name__):
                self.assertTrue(checker(mutated))

    def test_missing_scheduling_dependency_is_not_a_blanket_exception(self):
        text = WORKFLOW.read_text()
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['WindowsPackages'])
        broken = block.replace('id: native-ready', 'id: unrelated-ready')
        issues = QUALITY.ci_build_workflow_issues(text.replace(block, broken, 1))
        self.assertTrue(any('BuildAdbWindowsX64 must be an ancestor of WindowsPackages' in issue for issue in issues), issues)

    def test_windows_overlap_rule_rejects_near_misses_spoofs_and_malformed_bindings(self):
        text = WORKFLOW.read_text()
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['WindowsPackages'])
        changes = (
            ('--platform Windows', '--platform macOS'),
            ('--architecture x86_64', '--architecture arm64'),
            ('phase: prepare', 'phase: full'),
            ('native-receipt: ${{ runner.temp }}/native-ready.json', 'native-receipt: stale.json'),
            ('artifact-ids: ${{ steps.native-ready.outputs.adb-artifact-id }}', 'artifact-ids: 123'),
            ('artifact-ids: ${{ steps.native-ready.outputs.support-artifact-id }}',
             'artifact-ids: ${{ steps.native-ready.outputs.support-artifact-id }}\n          name: adb-helper-package-support'),
            ('id: native-ready', 'id: native-ready\n        continue-on-error: true'),
            ('id: native-ready', 'id: native-ready\n        id: spoofed'),
            ('python3 scripts/ci_native_rendezvous.py', 'echo python3 scripts/ci_native_rendezvous.py'),
            ('python3 scripts/ci_native_rendezvous.py', '# python3 scripts/ci_native_rendezvous.py'),
            ('python3 scripts/verify_adb_helper_envelope.py', 'echo python3 scripts/verify_adb_helper_envelope.py'),
            ('python3 scripts/verify_adb_helper_envelope.py', 'python3 "unterminated scripts/verify_adb_helper_envelope.py'),
            ('gh attestation verify', 'echo gh attestation verify'),
            ('if [ "$GITHUB_EVENT_NAME" != "pull_request" ]; then',
             'if [ "$GITHUB_EVENT_NAME" == "pull_request" ]; then'),
            ('name: Fail when tests fail', 'name: Ignore test failure'),
            ('name: Verify Windows native overlap', 'name: Skip native phase proof'),
            ('python -I tests/scripts/test_ci_native_overlap.py WindowsNativeOverlapTests',
             'echo python -I tests/scripts/test_ci_native_overlap.py WindowsNativeOverlapTests'),
        )
        for before, after in changes:
            with self.subTest(change=after):
                self.assertIn(before, block)
                mutated = text.replace(block, block.replace(before, after, 1), 1)
                self.assertTrue(QUALITY.native_overlap_workflow_issues(mutated, 'WindowsPackages', platform='Windows'))
        spoof = text.replace(block, block.replace('id: native-ready', '# id: native-ready', 1), 1)
        self.assertTrue(QUALITY.native_overlap_workflow_issues(spoof, 'WindowsPackages', platform='Windows'))
        self.assertTrue(QUALITY.native_overlap_workflow_issues('jobs:\n  WindowsPackages:\n    steps: invalid\n',
                                                              'WindowsPackages', platform='Windows'))

    def test_pr_push_dispatch_keep_package_gates_and_nonpackage_full_builds(self):
        text = WORKFLOW.read_text()
        triggers = QUALITY.workflow_trigger_mapping(text)
        blocks = QUALITY.workflow_job_blocks(text)
        for event in ('pull_request', 'push', 'workflow_dispatch'):
            for job, package in (('WindowsPackages', True), ('WindowsX86', False), ('WindowsAsan', False)):
                with self.subTest(event=event, job=job):
                    self.assertIn(event, triggers)
                    values = QUALITY.workflow_job_condition_values(blocks[job])
                    self.assertEqual(values['klogg_package_enabled'], {'true' if package else 'false'})
                    rows = steps(text, job)
                    prepare = next(fields for fields, children in rows
                                   if children.get('with', {}).get('phase') == 'prepare')
                    ready = next(fields for fields, _ in rows if fields.get('id') == 'native-ready')
                    for fields in (prepare, ready):
                        self.assertEqual(fields['if'], "${{ env.KLOGG_PACKAGE_ENABLED == 'true' }}")
                        self.assertEqual(QUALITY.artifact_condition_is_statically_false(fields['if'], values), not package)
                    final = next(children['with'] for fields, children in rows if fields.get('name') == 'Build Windows application')
                    self.assertEqual(final['phase'], "${{ env.KLOGG_PACKAGE_ENABLED == 'true' && 'finalize' || 'full' }}")
                    verify = next(fields for fields, _ in rows if fields.get('name') == 'Verify target-bound ADB helper checksum envelope and provenance')
                    self.assertIn('gh attestation verify "$artifact_root/SHA256SUMS"', verify['run'])
                    self.assertIn('if [ "$GITHUB_EVENT_NAME" != "pull_request" ]; then', verify['run'])


class SharedApplicationObjectsTest(unittest.TestCase):
    def test_windows_descriptor_names_match_every_actual_producer_job(self):
        text = WORKFLOW.read_text()
        blocks = QUALITY.workflow_job_blocks(text)
        for job in ('WindowsPackages', 'WindowsX86', 'WindowsAsan'):
            with self.subTest(job=job):
                block = '\n'.join(blocks[job])
                environment = dict(re.findall(r'^      (KLOGG_\w+): (.+)$', block, re.M))
                descriptor = next(fields['run'] for fields, _ in steps(text, job)
                                  if fields.get('id') == 'shared-cache-descriptor')
                producer = re.search(r'--producer-job "([^"]+)"', descriptor).group(1)
                producer = re.sub(r'\$(KLOGG_\w+)',
                                  lambda match: environment[match.group(1)].strip('"'), producer)
                display = QUALITY.workflow_job_direct_value(blocks[job], 'name').rsplit('}}', 1)[-1]
                self.assertEqual(producer, display)

    def test_all_enabled_platforms_restore_shared_objects_before_build_and_export_after_gates(self):
        for job, backend, build_action, test_action, package_action in (
            ('LinuxPackages', '.ccache', 'docker-build', 'docker-run-tests', 'docker-package'),
            ('MacPackages', '.ccache', 'agent-build', 'agent-run-tests', 'agent-package-mac'),
            ('WindowsPackages', '.sccache', 'agent-build', 'agent-run-tests', 'agent-package-win'),
        ):
            with self.subTest(job=job):
                rows = steps(WORKFLOW.read_text(), job)
                descriptors = [i for i, (fields, _) in enumerate(rows) if fields.get('id') == 'shared-cache-descriptor']
                self.assertEqual(len(descriptors), 1)
                restore = next(i for i, (fields, _) in enumerate(rows)
                               if fields.get('uses') == './.github/actions/restore-compiler-object-artifact')
                export = next(i for i, (fields, _) in enumerate(rows)
                              if fields.get('uses') == './.github/actions/export-compiler-object-artifact')
                builds = [i for i, (fields, _) in enumerate(rows)
                          if fields.get('uses') == './.github/actions/' + build_action]
                tests = next(i for i, (fields, _) in enumerate(rows)
                             if fields.get('uses') == './.github/actions/' + test_action)
                package = next(i for i, (fields, _) in enumerate(rows)
                               if fields.get('uses') == './.github/actions/' + package_action)
                self.assertLess(descriptors[0], restore)
                self.assertLess(restore, min(builds))
                self.assertLess(max(builds), tests)
                self.assertLess(tests, package)
                self.assertLess(package, export)
                for i in (restore, export):
                    self.assertIn("steps.shared-cache-descriptor.outputs.eligible == 'true'", rows[i][0]['if'])
                    self.assertEqual(rows[i][1]['with']['path'], '${{ github.workspace }}/' + backend)
                self.assertIn('success()', rows[export][0]['if'])
                self.assertEqual(rows[export][1]['with']['export-needed'], '${{ steps.restore-shared-compiler-cache.outputs.export-needed }}')
                self.assertIn('scripts/ci_compiler_object_descriptor.py', rows[descriptors[0]][0]['run'])
                for fields, children in rows:
                    if fields.get('uses') == './.github/actions/save-verified-compiler-cache':
                        self.assertEqual(children['with'].get('descriptor'),
                                         '${{ steps.shared-cache-descriptor.outputs.descriptor }}')
                if job.startswith('Windows'):
                    activate = next(i for i, (fields, _) in enumerate(rows) if fields.get('id') == 'activate-windows-cache')
                    self.assertLess(restore, activate)


class MacTsanOptimizationTest(unittest.TestCase):
    @staticmethod
    def configure_with_workflow_options(text, workspace):
        """Execute the actual option-producing shell steps, then nested CMake."""
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['MacSanitizers'])
        configurations = block.split('        config:\n', 1)[1].split('    runs-on:', 1)[0]
        tsan = next(row for row in configurations.split('          - os:')[1:]
                    if 'label: intel-qt6-first-party-tsan' in row)
        matrix_options = re.search(r'^\s+cmake_opts: (.+)$', tsan, re.M).group(1)
        substitutions = {
            'env.KLOGG_PACKAGE_ENABLED': 'false',
            'env.KLOGG_ADB_HELPER_TARGET': 'macos-x86_64',
            'env.KLOGG_CONFIG_CMAKE_OPTS': matrix_options,
            'github.workspace': str(workspace),
        }
        env = dict(os.environ, KLOGG_WORKSPACE=str(workspace),
                   KLOGG_PACKAGE_ENABLED='false', KLOGG_SANITIZER='thread',
                   RUNNER_OS='macOS', GITHUB_ENV=str(workspace / 'github-env'))
        developer = Path('/Applications/Xcode_26.6.app/Contents/Developer')
        if developer.is_dir():
            env['DEVELOPER_DIR'] = str(developer)
        for fields, _ in steps(text, 'MacSanitizers'):
            if fields.get('id') == 'shared-cache-descriptor':
                break
            if fields.get('name') not in (
                'Disable Sentry on macOS', 'Select Intel macOS TSan optimization profile'
            ):
                continue
            script = re.sub(r'\$\{\{\s*(.*?)\s*\}\}',
                            lambda match: substitutions[match.group(1)], fields['run'])
            subprocess.run([fields['shell'], '-eu', '-c', script], env=env,
                           cwd=workspace, check=True, capture_output=True, text=True)
            # GitHub imports GITHUB_ENV values between steps without shell eval.
            for line in Path(env['GITHUB_ENV']).read_text().splitlines():
                name, value = line.split('=', 1)
                env[name] = value
        # The action runs CMake from its build directory, not from the checkout.
        build = workspace / 'build'
        build.mkdir()
        command = ['cmake', *shlex.split(env['KLOGG_CMAKE_OPTS']),
                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', str(workspace)]
        result = subprocess.run(command, cwd=build, env=env, check=False,
                                capture_output=True, text=True)
        return result, env['KLOGG_CMAKE_OPTS']

    @staticmethod
    def make_nested_project(workspace):
        (workspace / 'cmake').mkdir()
        shutil.copy2(ROOT / 'cmake/CiMacosTsanProfile.cmake', workspace / 'cmake')
        (workspace / 'dependency').mkdir()
        (workspace / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.14)\n'
            'project(workflow_profile_fixture LANGUAGES C CXX)\n'
            'set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g1 -DNDEBUG")\n'
            'add_compile_options(-fsanitize=thread)\n'
            'add_subdirectory(dependency)\n'
        )
        (workspace / 'dependency/CMakeLists.txt').write_text(
            'project(nested_dependency LANGUAGES C)\n'
            'add_library(dependency STATIC dep.c)\n'
            'target_compile_options(dependency PRIVATE -O3)\n'
        )
        (workspace / 'dependency/dep.c').write_text('int dependency(void) { return 7; }\n')

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'x86_64',
                         'the enabled profile requires Intel macOS AppleClang')
    def test_actual_workflow_options_configure_nested_dependency_projects(self):
        with tempfile.TemporaryDirectory(prefix='klogg-workflow-profile-') as directory:
            workspace = Path(directory)
            self.make_nested_project(workspace)
            result, options = self.configure_with_workflow_options(WORKFLOW.read_text(), workspace)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            profile = next(option.split('=', 1)[1] for option in shlex.split(options)
                           if option.startswith('-DCMAKE_PROJECT_INCLUDE='))
            self.assertTrue(Path(profile).is_absolute(), profile)
            self.assertEqual(Path(profile), workspace / 'cmake/CiMacosTsanProfile.cmake')
            commands = json.loads((workspace / 'build/compile_commands.json').read_text())
            self.assertEqual(len(commands), 1)
            arguments = shlex.split(commands[0]['command'])
            self.assertEqual([arg for arg in arguments if arg.startswith('-O')][-1], '-O1')
            self.assertIn('-g1', arguments)
            self.assertIn('-fsanitize=thread', arguments)

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'x86_64',
                         'the enabled profile requires Intel macOS AppleClang')
    def test_relative_workflow_hook_mutation_fails_in_nested_project(self):
        text = WORKFLOW.read_text()
        absolute = '-DCMAKE_PROJECT_INCLUDE=$KLOGG_WORKSPACE/cmake/CiMacosTsanProfile.cmake'
        self.assertEqual(text.count(absolute), 1)
        broken = text.replace(absolute, '-DCMAKE_PROJECT_INCLUDE=cmake/CiMacosTsanProfile.cmake')
        with tempfile.TemporaryDirectory(prefix='klogg-workflow-relative-') as directory:
            workspace = Path(directory)
            self.make_nested_project(workspace)
            result, _ = self.configure_with_workflow_options(broken, workspace)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('dependency/CMakeLists.txt:1 (project)', result.stderr)
            self.assertIn('project could not find requested file', result.stderr)

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'x86_64',
                         'the enabled profile requires Intel macOS AppleClang')
    def test_workflow_profile_selection_fails_closed_if_hook_is_missing(self):
        with tempfile.TemporaryDirectory(prefix='klogg-workflow-missing-') as directory:
            workspace = Path(directory)
            self.make_nested_project(workspace)
            (workspace / 'cmake/CiMacosTsanProfile.cmake').unlink()
            with self.assertRaises(subprocess.CalledProcessError):
                self.configure_with_workflow_options(WORKFLOW.read_text(), workspace)

    def test_only_intel_tsan_enables_measured_profile_and_checks_generated_commands(self):
        text = WORKFLOW.read_text()
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['MacSanitizers'])
        configurations = block.split('        config:\n', 1)[1].split('    runs-on:', 1)[0]
        tsan = next(row for row in configurations.split('          - os:')[1:]
                    if 'label: intel-qt6-first-party-tsan' in row)
        option = '-DKLOGG_CI_MACOS_TSAN_O1=ON'
        self.assertIn(option, tsan)
        self.assertEqual(text.count(option), 1)
        self.assertNotIn('CMAKE_PROJECT_INCLUDE', configurations)
        workflow_steps = steps(text, 'MacSanitizers')
        names = [fields.get('name') for fields, _ in workflow_steps]
        selection = names.index('Select Intel macOS TSan optimization profile')
        constructor = names.index('Disable Sentry on macOS')
        descriptor = next(index for index, (fields, _) in enumerate(workflow_steps)
                          if fields.get('id') == 'shared-cache-descriptor')
        self.assertLess(constructor, selection)
        self.assertLess(selection, descriptor)
        self.assertEqual(workflow_steps[selection][0]['if'],
                         "${{ runner.os == 'macOS' && env.KLOGG_SANITIZER == 'thread' }}")
        self.assertEqual(workflow_steps[selection][0]['shell'], 'sh')
        self.assertNotRegex(workflow_steps[selection][0]['run'], r'\beval\b')
        action = (ROOT / '.github/actions/agent-build/action.yml').read_text()
        body = action.split('  steps:\n', 1)[1]
        wrapped = 'jobs:\n  NativeBuild:\n    steps:\n' + '\n'.join('  ' + line for line in body.splitlines())
        rows = steps(wrapped, 'NativeBuild')
        names = [fields.get('name') for fields, _ in rows]
        guard = names.index('Verify generated macOS TSan optimization profile')
        self.assertLess(names.index('configure'), guard)
        self.assertLess(guard, names.index('build'))
        self.assertEqual(rows[guard][0], {
            'name': 'Verify generated macOS TSan optimization profile',
            'if': "${{ inputs.phase == 'full' && runner.os == 'macOS' && env.KLOGG_SANITIZER == 'thread' }}",
            'shell': 'sh',
            'run': 'python3 scripts/verify_macos_tsan_profile.py "$KLOGG_BUILD_ROOT/compile_commands.json"',
        })


if __name__ == '__main__':
    unittest.main()
