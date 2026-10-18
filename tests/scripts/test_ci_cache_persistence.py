"""Compiler cache publication must be confirmed, not inferred from action success."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock

from test_ci_environment_workflow import QUALITY as LINT

ROOT = Path(__file__).resolve().parents[2]
ACTION = './.github/actions/save-verified-compiler-cache'
PRIMARY = 'linux-ccache-v2-2026-10-18'
REF = 'refs/heads/master'


def entry(key=PRIMARY, ref=REF, size=100, identifier=1):
    return {'id': identifier, 'key': key, 'ref': ref, 'size_in_bytes': size}


def pages(*entries):
    return [{'total_count': len(entries), 'actions_caches': list(entries)}]


def load_module():
    spec = importlib.util.spec_from_file_location('ci_cache_persistence', ROOT / 'scripts/ci_cache_persistence.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CachePersistenceWorkflowTest(unittest.TestCase):
    def test_application_compiler_saves_require_backend_confirmation(self):
        jobs = LINT.workflow_job_steps((ROOT / '.github/workflows/ci-build.yml').read_text())
        for job in ('LinuxPackages', 'MacPackages', 'WindowsPackages'):
            with self.subTest(job=job):
                steps = [LINT.workflow_step_fields(step)[0] for step in jobs[job]]
                saves = [s for s in steps if s.get('name', '').startswith(
                    ('Save ccache', 'Save own-PR ccache', 'Save validated Windows'))]
                self.assertTrue(saves)
                for step in saves:
                    self.assertEqual(step.get('uses'), ACTION)


class CachePersistenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_lookup_and_rest_disagreement_is_unconfirmed_not_missing(self):
        for matched, response in (('', pages(entry())), (PRIMARY, pages())):
            with self.subTest(matched=matched, response=response):
                result = self.module.probe('owner/repo', PRIMARY, REF, matched_key=matched,
                                           fetch=mock.Mock(return_value=response))
                self.assertEqual(result, {'state': 'unconfirmed', 'persisted-key': ''})

    def test_rest_metadata_without_current_version_lookup_is_not_persistence(self):
        previous_version = {**entry(), 'version': 'old-path-or-compression-version'}
        self.assertEqual(self.module.select_cache(pages(previous_version), PRIMARY, REF)['state'], 'unconfirmed')
        self.assertEqual(self.module.select_cache(pages(previous_version), PRIMARY, REF,
                                                 matched_key='')['state'], 'unconfirmed')
        self.assertEqual(self.module.select_cache(pages(entry()), PRIMARY, REF,
                                                 matched_key=PRIMARY)['persisted-key'], PRIMARY)
        recovery = PRIMARY + '-recovery-123-2'
        self.assertEqual(self.module.select_cache(pages(previous_version, entry(key=recovery, identifier=2)),
                                                 PRIMARY, REF, matched_key=recovery)['persisted-key'], recovery)
        for matched in (PRIMARY + '-other', PRIMARY + '-recovery-1-1-extra', 'key\nstate=persisted'):
            with self.subTest(matched=matched), self.assertRaises(ValueError):
                self.module.select_cache(pages(entry()), PRIMARY, REF, matched_key=matched)

    def test_exact_ref_and_key_with_positive_size_are_required(self):
        for candidate in (entry(ref='refs/pull/4/merge'), entry(key=PRIMARY + '-suffix'),
                          entry(size=0), entry(key=PRIMARY + '-recovery-1-1-extra'),
                          entry(key=PRIMARY + '-recovery-0-1')):
            with self.subTest(candidate=candidate):
                self.assertEqual(self.module.select_cache(pages(candidate), PRIMARY, REF,
                                                         matched_key=PRIMARY)['state'], 'unconfirmed')
                self.assertEqual(self.module.select_cache(pages(candidate), PRIMARY, REF)['state'], 'missing')
        self.assertEqual(self.module.select_cache(pages(entry()), PRIMARY, REF,
                                                 matched_key=PRIMARY)['persisted-key'], PRIMARY)

    def test_existing_primary_or_daily_recovery_is_reused_idempotently(self):
        recovery = PRIMARY + '-recovery-123-2'
        fixtures = ((pages(entry()), PRIMARY),
                    (pages(entry(key=recovery)), recovery),
                    (pages(entry(key=recovery), entry(identifier=2)), PRIMARY))
        for response, expected in fixtures:
            with self.subTest(expected=expected):
                fetch = mock.Mock(return_value=response)
                for _ in range(2):
                    result = self.module.probe('owner/repo', PRIMARY, REF, matched_key=expected, fetch=fetch)
                    self.assertEqual(result, {'state': 'persisted', 'persisted-key': expected})
                self.assertEqual(fetch.call_count, 2)

    def test_malformed_or_incomplete_pagination_is_unknown_not_missing(self):
        malformed = [None, {}, [], [None], [{'total_count': 0}],
                     [{'total_count': True, 'actions_caches': []}],
                     [{'total_count': -1, 'actions_caches': []}],
                     [{'total_count': 1, 'actions_caches': []}],
                     [{'total_count': 0, 'actions_caches': [entry()]}],
                     pages(None), pages({}), pages(entry(size=True)), pages(entry(size=-1)),
                     pages(entry(key=None)), pages(entry(ref=None)), pages(entry(identifier=True)),
                     pages(entry(), entry()),
                     [{'total_count': 2, 'actions_caches': [entry()]},
                      {'total_count': 2, 'actions_caches': [entry(identifier=2)]}],
                     pages(entry()) + pages()]
        for response in malformed:
            with self.subTest(response=response), self.assertRaises(ValueError):
                self.module.select_cache(response, PRIMARY, REF)
        first = [entry(key='other', identifier=index + 1) for index in range(100)]
        response = [{'total_count': 101, 'actions_caches': first},
                    {'total_count': 101, 'actions_caches': [entry(identifier=101)]}]
        self.assertEqual(self.module.select_cache(response, PRIMARY, REF, matched_key=PRIMARY)['persisted-key'], PRIMARY)
        response[1]['total_count'] = 102
        with self.assertRaises(ValueError):
            self.module.select_cache(response, PRIMARY, REF)

    def test_single_observation_never_retries_with_frozen_lookup_proof(self):
        fetch = mock.Mock(return_value=pages())
        self.assertEqual(self.module.probe('owner/repo', PRIMARY, REF, fetch=fetch),
                         {'state': 'missing', 'persisted-key': ''})
        fetch.assert_called_once()
        for error in (ValueError('malformed API'), subprocess.TimeoutExpired('gh', 10),
                      subprocess.CalledProcessError(1, ['gh', 'api'])):
            fetch = mock.Mock(side_effect=error)
            with self.subTest(error=error), self.assertRaises(type(error)):
                self.module.probe('owner/repo', PRIMARY, REF, fetch=fetch)
            fetch.assert_called_once()

    def test_read_only_api_uses_escaped_filters_complete_pages_and_timeout(self):
        result = subprocess.CompletedProcess([], 0, stdout=json.dumps(pages()), stderr='')
        with mock.patch.object(self.module.subprocess, 'run', return_value=result) as runner:
            self.assertEqual(self.module.fetch_caches('owner/repo', PRIMARY, REF), pages())
        command = runner.call_args.args[0]
        self.assertEqual(command[:2], ['gh', 'api'])
        self.assertIn('repos/owner/repo/actions/caches?', command[2])
        self.assertIn('ref=refs%2Fheads%2Fmaster', command[2])
        self.assertIn('key=' + PRIMARY, command[2])
        self.assertIn('per_page=100', command[2])
        self.assertEqual(command[3:], ['--paginate', '--slurp'])
        self.assertEqual(runner.call_args.kwargs,
                         {'capture_output': True, 'text': True, 'check': True, 'timeout': 10})
        for repository in ('../owner/repo', 'owner/repo/extra', 'https://evil.test'):
            with self.assertRaises(ValueError):
                self.module.fetch_caches(repository, PRIMARY, REF)

    def test_scope_rejects_invalid_refs_without_requesting_the_api(self):
        for ref in ('master', 'refs/heads/../master', 'refs/heads/master\n',
                    'refs/heads/a//b', 'refs/pull/0/merge', 'refs/heads/a.lock'):
            with self.subTest(ref=ref), mock.patch.object(self.module.subprocess, 'run') as runner:
                with self.assertRaises(ValueError):
                    self.module.fetch_caches('owner/repo', PRIMARY, ref)
                runner.assert_not_called()
        for ref in ('refs/heads/topic/cache', 'refs/pull/42/merge', 'refs/tags/v1.0'):
            self.module.validate_scope('owner/repo', PRIMARY, ref)

    def test_cli_requires_final_persistence_and_never_emits_missing_on_unknown(self):
        cases = [(json.dumps(pages(entry())), PRIMARY, False, 0, 'persisted'),
                 (json.dumps(pages()), '', False, 0, 'missing'),
                 (json.dumps(pages()), '', True, 1, ''),
                 (json.dumps(pages(entry())), '', False, 0, 'unconfirmed'),
                 (json.dumps(pages(entry())), '', True, 1, ''),
                 ('not-json', '', False, 1, ''),
                 (json.dumps([{'total_count': 1, 'actions_caches': []}]), '', False, 1, ''),
                 (subprocess.CalledProcessError(1, ['gh', 'api']), '', False, 1, ''),
                 (subprocess.TimeoutExpired('gh', 10), '', False, 1, '')]
        for response, matched, require, expected, state in cases:
            with self.subTest(response=response, require=require), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / 'output'
                output.touch()
                env = {'GITHUB_REPOSITORY': 'owner/repo', 'GITHUB_REF': REF,
                       'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '2',
                       'GITHUB_OUTPUT': str(output)}
                command = ['--key', PRIMARY, '--matched-key', matched]
                command += ['--require-persisted'] if require else []
                with mock.patch.dict(self.module.os.environ, env, clear=True), \
                        mock.patch.object(self.module.subprocess, 'run') as runner:
                    if isinstance(response, Exception):
                        runner.side_effect = response
                    else:
                        runner.return_value = subprocess.CompletedProcess([], 0, stdout=response, stderr='')
                    self.assertEqual(self.module.main(command), expected)
                if state:
                    self.assertIn('state=' + state + '\n', output.read_text())
                    self.assertIn('recovery-key=' + PRIMARY + '-recovery-123-2\n', output.read_text())
                else:
                    self.assertEqual(output.read_text(), '')
                runner.assert_called_once()

    def test_recovery_keys_are_bounded_unique_and_cannot_inject_outputs(self):
        self.assertEqual(self.module.recovery_key(PRIMARY, '123', '2'), PRIMARY + '-recovery-123-2')
        for key, run, attempt in ((PRIMARY, '0', '1'), (PRIMARY, '1\nstate=missing', '1'),
                                  (PRIMARY, '1', '0'), ('key\n', '1', '1'),
                                  ('x' * 512, '1', '1'), ('', '1', '1')):
            with self.subTest(key=key, run=run, attempt=attempt), self.assertRaises(ValueError):
                self.module.recovery_key(key, run, attempt)


class CachePersistenceActionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def action(self, name='save-verified-compiler-cache', text=None):
        if text is None:
            text = (ROOT / '.github/actions' / name / 'action.yml').read_text()
        workflow = 'jobs:\n  save:\n' + '\n'.join(
            '  ' + line for line in text.split('runs:\n', 1)[1].splitlines())
        blocks = LINT.workflow_job_steps(workflow)['save']
        return {fields['id']: (fields, children)
                for fields, children in map(LINT.workflow_step_fields, blocks)}

    def expression(self, text, outputs, *, inputs=None, successful=True):
        value = re.sub(r'steps\.([\w-]+)\.outputs\.([\w-]+)',
                       lambda match: repr(outputs.get(match[1], {}).get(match[2], '')), text[3:-2].strip())
        value = re.sub(r'inputs\.([\w-]+)', lambda match: repr((inputs or {}).get(match[1], '')), value)
        value = value.replace('success()', str(successful))
        return eval(value.replace('true', 'True').replace('&&', ' and ').replace('||', ' or '),
                    {'__builtins__': {}}, {})

    def project(self, initial, primary, recovery, fail=None, *, descriptor=''):
        outputs, saved, successful = {}, [], True
        for identifier, (fields, children) in self.action().items():
            if not successful or not self.expression(fields.get('if', '${{ true }}'), outputs,
                                                     inputs={'descriptor': descriptor}, successful=successful):
                continue
            if identifier == 'mark-backend':
                if fail == identifier:
                    successful = False
            elif identifier in ('save-primary', 'save-recovery'):
                saved.append(identifier)
                if fail == identifier and fields.get('continue-on-error') != 'true':
                    successful = False
            else:
                state = {'inspect': initial, 'verify-primary': primary, 'verify-recovery': recovery}[identifier]
                required = children['with'].get('require-persisted') == 'true'
                if state in ('unknown', 'unconfirmed') or (required and state != 'persisted'):
                    successful = False
                else:
                    outputs[identifier] = {'state': state}
        return successful, saved

    def project_probe(self, observations, *, required=False, text=None):
        outputs, successful, lookups, requests = {}, True, 0, 0
        observations = iter(observations)
        for identifier, (fields, children) in self.action('probe-compiler-cache', text).items():
            if not successful or not self.expression(fields.get('if', '${{ true }}'), outputs):
                continue
            if fields.get('uses', '').startswith('actions/cache/restore@'):
                matched, response = next(observations)
                lookups += 1
                outputs[identifier] = {'cache-matched-key': matched}
            elif identifier in ('inspect', 'inspect-retry'):
                matched = self.expression(children['env']['CACHE_MATCHED_KEY'], outputs)
                fetch = mock.Mock(side_effect=response) if isinstance(response, Exception) else mock.Mock(return_value=response)
                requests += 1
                try:
                    outputs[identifier] = self.module.probe('owner/repo', PRIMARY, REF,
                                                            matched_key=matched, fetch=fetch)
                except (ValueError, subprocess.SubprocessError):
                    successful = False
            elif identifier == 'require':
                state = self.expression(children['env']['CACHE_STATE'], outputs)
                result = subprocess.run(['bash', '-c', fields['run']], capture_output=True, text=True,
                                        env={'CACHE_STATE': state, 'REQUIRE_PERSISTED': str(required).lower()})
                successful = result.returncode == 0
            else:
                self.assertEqual(identifier, 'retry-delay')
                self.assertEqual(fields['run'].strip(), 'sleep 2')
        state = outputs.get('inspect-retry', outputs.get('inspect', {})).get('state', 'unknown')
        return successful, state, lookups, requests

    def test_fresh_lookup_retry_observes_late_visibility_without_recovery_save(self):
        observations = [('', pages(entry())), (PRIMARY, pages(entry()))]
        outcome = self.project_probe(observations)
        self.assertEqual(outcome, (True, 'persisted', 2, 2))
        self.assertEqual(self.project('missing', outcome[1], 'missing'), (True, ['save-primary']))
        # Reproduce the prior frozen-proof defect without modifying repository files.
        text = (ROOT / '.github/actions/probe-compiler-cache/action.yml').read_text()
        frozen = text.replace('steps.lookup-retry.outputs.cache-matched-key',
                              'steps.lookup.outputs.cache-matched-key')
        self.assertNotEqual(frozen, text)
        self.assertEqual(self.project_probe(observations, text=frozen), (False, 'unconfirmed', 2, 2))
        self.assertEqual(self.project_probe([(PRIMARY, pages()), (PRIMARY, pages(entry()))], required=True),
                         (True, 'persisted', 2, 2))

    def test_probe_retries_are_bounded_and_ambiguous_or_api_error_evidence_never_authorizes_save(self):
        for required in (False, True):
            with self.subTest(required=required):
                self.assertEqual(self.project_probe([('', pages(entry()))] * 2, required=required),
                                 (False, 'unconfirmed', 2, 2))
                self.assertEqual(self.project_probe([('', pages())] * 2, required=required),
                                 (not required, 'missing', 2, 2))
        self.assertEqual(self.project_probe([(PRIMARY, pages(entry()))]), (True, 'persisted', 1, 1))
        for error in (ValueError('malformed API'), subprocess.TimeoutExpired('gh', 10)):
            self.assertEqual(self.project_probe([('', error)]), (False, 'unknown', 1, 1))

    def test_actual_predicates_allow_only_confirmed_absence_and_one_recovery(self):
        scenarios = [
            ('persisted', 'missing', 'missing', None, (True, [])),
            ('unknown', 'missing', 'missing', None, (False, [])),
            ('unconfirmed', 'missing', 'missing', None, (False, [])),
            ('missing', 'persisted', 'missing', None, (True, ['save-primary'])),
            ('missing', 'unknown', 'missing', None, (False, ['save-primary'])),
            ('missing', 'unconfirmed', 'missing', None, (False, ['save-primary'])),
            ('missing', 'missing', 'persisted', 'save-primary', (True, ['save-primary', 'save-recovery'])),
            ('missing', 'missing', 'persisted', 'save-recovery', (True, ['save-primary', 'save-recovery'])),
            ('missing', 'missing', 'missing', None, (False, ['save-primary', 'save-recovery'])),
            ('missing', 'missing', 'unknown', None, (False, ['save-primary', 'save-recovery'])),
            ('missing', 'missing', 'unconfirmed', None, (False, ['save-primary', 'save-recovery'])),
        ]
        for initial, primary, recovery, fail, expected in scenarios:
            for descriptor in ('', 'current-descriptor.json'):
                with self.subTest(initial=initial, primary=primary, recovery=recovery, fail=fail, descriptor=descriptor):
                    self.assertEqual(self.project(initial, primary, recovery, fail, descriptor=descriptor), expected)

    def test_probe_refreshes_both_sources_with_identical_version_inputs(self):
        steps = self.action('probe-compiler-cache')
        self.assertEqual(list(steps), ['lookup', 'inspect', 'retry-delay', 'lookup-retry', 'inspect-retry', 'require'])
        for lookup, verify in (('lookup', 'inspect'), ('lookup-retry', 'inspect-retry')):
            fields, children = steps[lookup]
            self.assertEqual(fields['uses'], 'actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9')
            self.assertNotIn('continue-on-error', fields)
            self.assertEqual(children['with'], {'path': '${{ inputs.path }}', 'key': '${{ inputs.key }}',
                                               'restore-keys': '${{ inputs.key }}-recovery-', 'lookup-only': 'true'})
            self.assertEqual(steps[verify][1]['env']['CACHE_MATCHED_KEY'],
                             '${{ steps.' + lookup + '.outputs.cache-matched-key }}')
            self.assertIn('--matched-key "$CACHE_MATCHED_KEY"', steps[verify][0]['run'])
            self.assertNotIn('--checks', steps[verify][0]['run'])
        for fields, _ in steps.values():
            self.assertNotIn('continue-on-error', fields)
            self.assertNotIn('always()', fields.get('if', ''))

    def test_optional_backend_marker_runs_only_at_successful_save_boundary(self):
        steps = self.action()
        self.assertIn('mark-backend', steps)
        fields, children = steps['mark-backend']
        self.assertEqual(fields['if'], "${{ success() && inputs.descriptor != '' }}")
        self.assertNotIn('continue-on-error', fields)
        self.assertIn('python3 -I scripts/ci_compiler_object_transport.py mark-backend', fields['run'])
        self.assertEqual(children['env']['KLOGG_COMPILER_DESCRIPTOR'], '${{ inputs.descriptor }}')
        self.assertEqual(children['env']['KLOGG_COMPILER_CACHE'], '${{ inputs.path }}')
        self.assertFalse(self.expression(fields['if'], {}, inputs={'descriptor': ''}))
        self.assertFalse(self.expression(fields['if'], {}, inputs={'descriptor': 'ready.json'}, successful=False))
        self.assertEqual(self.project('missing', 'persisted', 'missing', 'mark-backend', descriptor='ready.json'), (False, []))
        text = (ROOT / '.github/actions/save-verified-compiler-cache/action.yml').read_text()
        inputs = LINT.workflow_mapping_block(text.splitlines(), 'inputs', 0)
        option = LINT.workflow_mapping_block(inputs['descriptor'][1], 'descriptor', 2)
        self.assertEqual(option['required'][0], 'false')
        self.assertEqual(option['default'][0], '')

    def test_pinned_save_steps_never_supply_authoritative_outputs(self):
        steps = self.action()
        self.assertEqual(list(steps), ['mark-backend', 'inspect', 'save-primary', 'verify-primary', 'save-recovery', 'verify-recovery'])
        for identifier in ('inspect', 'verify-primary', 'verify-recovery'):
            fields, children = steps[identifier]
            self.assertEqual(fields['uses'], './.github/actions/probe-compiler-cache')
            self.assertNotIn('continue-on-error', fields)
            expected = {'path': '${{ inputs.path }}', 'key': '${{ inputs.key }}',
                        'github-token': '${{ inputs.github-token }}'}
            if identifier == 'verify-recovery':
                expected['require-persisted'] = 'true'
            self.assertEqual(children['with'], expected)
        for identifier in ('save-primary', 'save-recovery'):
            fields, children = steps[identifier]
            self.assertEqual(fields['uses'], 'actions/cache/save@55cc8345863c7cc4c66a329aec7e433d2d1c52a9')
            self.assertEqual(fields['continue-on-error'], 'true')
            self.assertEqual(set(children['with']), {'path', 'key'})
            self.assertEqual(children['with']['path'], '${{ inputs.path }}')
        self.assertEqual(steps['save-primary'][1]['with']['key'], '${{ inputs.key }}')
        self.assertEqual(steps['save-recovery'][1]['with']['key'], '${{ steps.inspect.outputs.recovery-key }}')
        for fields, _ in steps.values():
            self.assertNotIn('always()', fields.get('if', ''))
        text = (ROOT / '.github/actions/save-verified-compiler-cache/action.yml').read_text()
        outputs = LINT.workflow_mapping_block(text.splitlines(), 'outputs', 0)
        self.assertEqual(set(outputs), {'state', 'persisted-key'})
        for name, (_, block) in outputs.items():
            fields = LINT.workflow_mapping_block(block, name, 2)
            expected = '${{ ' + ' || '.join('steps.' + step + '.outputs.' + name
                                            for step in ('verify-recovery', 'verify-primary', 'inspect')) + ' }}'
            self.assertEqual(fields['value'][0], expected)


if __name__ == '__main__':
    unittest.main()
