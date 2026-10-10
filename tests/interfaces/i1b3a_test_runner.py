#!/usr/bin/env python3
"""Fail-closed sentinels; every compiler and executable process is mocked."""
import contextlib
from dataclasses import replace
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import i1b3a_run as suite
from i1b3a_cases import matrix, inventory


class RunnerGuards(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.compiler = self.root/'compiler'
        self.compiler.write_text('inert compiler bytes, never executed')
        self.calls = []
        self.valid = next(c for c in matrix() if c.category == 'valid')
        self.reject = next(c for c in matrix() if c.category == 'reject')
        self.deferred = next(c for c in matrix() if c.category == 'deferred')
        self.addCleanup(self.temp.cleanup)

    def run_cases(self, cases, frozen=None, scenario='accepted'):
        frozen = inventory(cases) if frozen is None else frozen
        frozen = json.loads(json.dumps(frozen))
        def process(runner, command, stem):
            self.calls.append([str(x) for x in command])
            compile_step = str(command[0]) == str(self.compiler)
            stderr = ''
            code = 0
            if compile_step:
                out = Path(command[command.index('-o')+1])
                if scenario == 'accepted':
                    out.write_text('inert artifact, never executed')
                else:
                    case = cases[0]
                    line, col = case.expected_diagnostic_span
                    file = case.expected_diagnostic_file
                    if scenario == 'wrong-span':
                        col += 1
                    elif scenario == 'wrong-file':
                        file = 'other.sts'
                    diagnostic = case.diagnostic if scenario != 'wrong-diagnostic' else 'some other rejection'
                    stderr = f'error: {diagnostic}\n  --> {file}:{line}:{col}\n'
                    code = 1
                    if scenario == 'surviving-artifact':
                        out.write_text('must cause failed rejection')
            result = dict(command=[str(x) for x in command], category='exit',
                          returncode=code, stdout='', stderr=stderr)
            stem.with_suffix('.json').write_text(json.dumps(result))
            stem.with_suffix('.stdout').write_text('')
            stem.with_suffix('.stderr').write_text(stderr)
            return result
        with patch.object(suite, 'matrix', return_value=cases), \
             patch.object(suite, 'load_inventory', return_value=frozen), \
             patch.object(suite, 'LEVELS', (0,)), \
             patch.object(suite.Runner, 'process', process), \
             contextlib.redirect_stdout(io.StringIO()):
            failed = suite.run(self.compiler, self.root/'evidence')
        summary_path = next((self.root/'evidence').glob('*/summary.json'))
        return failed, json.loads(summary_path.read_text())

    def no_processes(self, cases, frozen=None):
        failed, summary = self.run_cases(cases, frozen)
        self.assertTrue(failed)
        self.assertTrue(summary['inventory_issues'])
        self.assertEqual(summary['results'], [])
        self.assertEqual(self.calls, [])

    def test_reject_reclassified_valid_drift_stops_before_process(self):
        changed = replace(self.reject, category='valid', diagnostic=None,
                          expected_span=None, seam_span=None,
                          expected_exit=0, expected_stdout='', expected_stderr='')
        self.no_processes([changed], inventory([self.reject]))

    def test_deferred_reclassified_valid_drift_stops_before_process(self):
        changed = replace(self.deferred, category='valid', expected_exit=0,
                          expected_stdout='', expected_stderr='')
        self.no_processes([changed], inventory([self.deferred]))

    def test_unknown_category_matching_inventory_stops_before_process(self):
        self.no_processes([replace(self.deferred, category='unknown')])

    def test_valid_missing_runtime_oracle_stops_before_process(self):
        self.no_processes([replace(self.valid, expected_exit=None)])

    def test_reject_runtime_oracle_stops_before_process(self):
        self.no_processes([replace(self.reject, expected_exit=0)])

    def test_deferred_diagnostic_stops_before_process(self):
        self.no_processes([replace(self.deferred, diagnostic='unexpected')])

    def test_reject_missing_span_stops_before_process(self):
        self.no_processes([replace(self.reject, expected_span=None)])

    def test_reject_missing_seam_stops_before_process(self):
        self.no_processes([replace(self.reject, seam_span=None)])

    def test_support_file_cannot_overwrite_primary_source(self):
        self.no_processes([replace(self.valid, files={'input.sts': 'mod changed;'})])

    def test_source_body_drift_stops_before_process(self):
        self.no_processes([replace(self.valid, source=self.valid.source+'\n// changed')],
                          inventory([self.valid]))

    def test_duplicate_inventory_stops_before_process(self):
        self.no_processes([self.valid], inventory([self.valid, self.valid]))

    def test_malformed_inventory_stops_before_process(self):
        self.no_processes([self.valid], {'not': 'a fixture list'})

    def test_support_file_escape_stops_before_process(self):
        self.no_processes([replace(self.valid, files={'../escape.sts': 'mod escape;'})])

    def test_accepted_negative_never_executes_and_fails(self):
        failed, summary = self.run_cases([self.reject])
        self.assertTrue(failed)
        self.assertTrue(summary['results'][0]['issues'])
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.calls[0][1], 'ir')

    def test_exact_rejection_succeeds_without_execution(self):
        failed, _ = self.run_cases([self.reject], scenario='correct-rejection')
        self.assertFalse(failed)
        self.assertEqual(len(self.calls), 1)

    def test_wrong_span_rejection_fails_without_execution(self):
        failed, summary = self.run_cases([self.reject], scenario='wrong-span')
        self.assertTrue(failed)
        self.assertIn('exact owned diagnostic file/span differs', summary['results'][0]['issues'])
        self.assertEqual(len(self.calls), 1)

    def test_wrong_file_rejection_fails_without_execution(self):
        failed, _ = self.run_cases([self.reject], scenario='wrong-file')
        self.assertTrue(failed)
        self.assertEqual(len(self.calls), 1)

    def test_wrong_diagnostic_fails_without_execution(self):
        failed, _ = self.run_cases([self.reject], scenario='wrong-diagnostic')
        self.assertTrue(failed)
        self.assertEqual(len(self.calls), 1)

    def test_surviving_negative_artifact_fails_without_execution(self):
        failed, _ = self.run_cases([self.reject], scenario='surviving-artifact')
        self.assertTrue(failed)
        self.assertEqual(len(self.calls), 1)

    def test_accepted_deferred_remains_compile_only(self):
        failed, _ = self.run_cases([self.deferred])
        self.assertFalse(failed)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.calls[0][1], 'ir')

    def test_valid_fixture_compiles_then_runs_mock_only(self):
        failed, _ = self.run_cases([self.valid])
        self.assertFalse(failed)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(self.calls[0][1], 'build')
        self.assertEqual(len(self.calls[1]), 1)


if __name__ == '__main__':
    unittest.main()
