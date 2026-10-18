"""Latency changes must preserve final native-artifact and publication gates."""
from pathlib import Path
import re
import unittest

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
    def test_only_intel_tsan_enables_measured_profile_and_checks_generated_commands(self):
        text = WORKFLOW.read_text()
        block = '\n'.join(QUALITY.workflow_job_blocks(text)['MacSanitizers'])
        configurations = block.split('        config:\n', 1)[1].split('    runs-on:', 1)[0]
        tsan = next(row for row in configurations.split('          - os:')[1:]
                    if 'label: intel-qt6-first-party-tsan' in row)
        for option in ('-DKLOGG_CI_MACOS_TSAN_O1=ON',
                       '-DCMAKE_PROJECT_INCLUDE=cmake/CiMacosTsanProfile.cmake'):
            self.assertIn(option, tsan)
            self.assertEqual(text.count(option), 1)
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
