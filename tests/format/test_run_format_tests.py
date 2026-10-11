"""Mocked fail-closed controls only; these never invoke a real compiler/program."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_format_tests as suite


class FormatRunnerControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / 'mock-compiler'
        self.compiler.write_text('mock compiler identity; never executed')
        self.cc = self.root / 'mock-cc'
        self.cc.write_text('mock native C compiler identity; never executed')
        self.helper_cc = self.root / 'mock-helper-cc'
        self.helper_cc.write_text('mock pure-helper C compiler identity; never executed')
        self.calls = []
        self.specs = {item['name']: item for kind in ('runtime', 'rejections')
                      for item in suite.inventory()[kind]}

    def runner(self, command, cwd, prefix, timeout, extra_env=None):
        self.calls.append((command, cwd, prefix, timeout, extra_env))
        process = dict(command=command, returncode=0, category='success', stdout='', stderr='')
        if command[0] in (str(self.cc), str(self.helper_cc)):
            output = Path(command[command.index('-o') + 1]); output.write_text('fresh C artifact')
        elif len(command) > 1:
            spec = self.specs[Path(command[2]).stem]
            if 'message' in spec:
                anchor = f"{Path(command[2]).name}:{spec['line']}"
                if spec['column']:
                    anchor += ':' + str(spec['column'])
                process.update(returncode=1, category='rejection',
                    stderr=f"error: {spec['message']}\n  --> {anchor}\n")
                logs = cwd / 'logs'; logs.mkdir()
                (logs / '.rotation-lock').write_text('')
                (logs / 'log0-last.log').write_text('ordinary driver log\n')
            else:
                output = Path(command[command.index('-o') + 1])
                text = 'fresh native executable'
                if command[1] == 'ir':
                    text = '''define void @promote_once(float %v) {
  %b = alloca [329 x i8]
  %d = fpext float %v to double
  %x = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %b, i64 329, ptr %f, double %d)
}
define void @direct_f64(double %v) {
  %b = alloca [328 x i8]
  %x = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %b, i64 328, ptr %f, double %v)
}
'''
                output.write_text(text)
        elif cwd.name.startswith('helper-'):
            process['stdout'] = 'PASS: 123 fixed format helper checks\n'
        else:
            name = 'float_precision_inline' if 'inline' in cwd.name else 'float_precision_native'
            process['stdout'] = self.specs[name]['stdout']
        return process

    def run_phase(self, phase, mutation=None, receipt=None):
        def runner(command, cwd, prefix, timeout, extra_env=None):
            process = self.runner(command, cwd, prefix, timeout, extra_env)
            if mutation:
                mutation(process, cwd, prefix)
            return process
        return suite.run_suite(phase, str(self.compiler), str(self.cc), artifacts=self.root,
                               ir_receipt=receipt, runner=runner, helper_cc=str(self.helper_cc))

    def ir_receipt(self):
        report, work = self.run_phase('ir')
        self.assertEqual((report['passed'], report['failed']), (8, 0), report)
        return work / 'results.json'

    def test_exact_mock_ir_native_and_helpers(self):
        receipt = self.ir_receipt()
        report, _ = self.run_phase('native', receipt=receipt)
        self.assertEqual((report['passed'], report['failed']), (5, 0), report)
        report, _ = self.run_phase('helper')
        self.assertEqual((report['passed'], report['failed']), (3, 0), report)
        self.assertEqual([item['check_count'] for item in report['results']], [123] * 3)
        self.assertEqual(report['helper_cc'], str(self.helper_cc))
        self.assertEqual(report['helper_cc_sha256'], suite.digest(self.helper_cc))
        self.assertEqual(report['cc_sha256'], suite.digest(self.cc))
        helper_compiles = [call for call in self.calls if call[2].name == 'compile'
                           and call[1].name.startswith('helper-')]
        self.assertEqual(len(helper_compiles), 3)
        self.assertTrue(all(call[0][0] == str(self.helper_cc) for call in helper_compiles))
        shim_compiles = [call for call in self.calls if call[1].name == 'support']
        self.assertEqual(len(shim_compiles), 1)
        self.assertEqual(shim_compiles[0][0][0], str(self.cc))
        self.assertTrue(all(call[4]['ASAN_OPTIONS'].startswith('detect_leaks=0:')
                            for call in self.calls if call[2].name == 'run' and call[1].name.startswith('helper-')))

    def test_wrong_stdout_or_stderr_fails(self):
        receipt = self.ir_receipt()
        for change in (dict(stdout='wrong\n'), dict(stdout=''), dict(stderr='unexpected\n')):
            def mutation(process, cwd, prefix):
                if prefix.name == 'run':
                    process.update(change)
            report, _ = self.run_phase('native', mutation, receipt)
            self.assertEqual(report['failed'], 4, report)

    def test_abnormal_exit_signal_timeout_or_sanitizer_fails(self):
        receipt = self.ir_receipt()
        for change in (dict(returncode=-11, category='crash'), dict(returncode=139, category='crash'),
                       dict(returncode=None, category='launch-error'), dict(returncode=0, category='timeout'),
                       dict(stderr='AddressSanitizer: failure'), dict(stderr='runtime error: failure')):
            def mutation(process, cwd, prefix):
                if prefix.name == 'run':
                    process.update(change)
            report, _ = self.run_phase('native', mutation, receipt)
            self.assertEqual(report['failed'], 4, report)

    def test_rejection_success_is_failure_and_never_runs(self):
        def mutation(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                process.update(returncode=0, category='success')
                (cwd / 'must-not-exist.ll').write_text('unexpected artifact')
        report, _ = self.run_phase('ir', mutation)
        self.assertEqual(report['failed'], 4, report)
        self.assertTrue(all(call[0][1] == 'ir' for call in self.calls))
        self.assertTrue(all(not item['runtime_executed'] for item in report['results']
                            if item['name'].startswith('reject-')))

    def test_rejection_diagnostic_and_anchor_exact(self):
        for replacement in ('wrong primary', ':4:99', ''):
            def mutation(process, cwd, prefix):
                if cwd.name.startswith('reject-'):
                    process['stderr'] = replacement
            report, _ = self.run_phase('ir', mutation)
            self.assertEqual(report['failed'], 4, report)

    def test_line_only_anchor_matches_complete_real_arrow_line(self):
        for suffix in (':0', ':27', ':unexpected-column', '0', ' trailing'):
            def mutation(process, cwd, prefix):
                if cwd.name.startswith('reject-'):
                    process['stderr'] = process['stderr'].rstrip('\n') + suffix + '\n'
            report, _ = self.run_phase('ir', mutation)
            self.assertEqual(report['failed'], 4, suffix)
        def wrong_basename(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                process['stderr'] = process['stderr'].replace('--> ', '--> other-')
        report, _ = self.run_phase('ir', wrong_basename)
        self.assertEqual(report['failed'], 4)
        def full_path(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                process['stderr'] = process['stderr'].replace('--> ', '--> ' + str(cwd) + '/')
        report, _ = self.run_phase('ir', full_path)
        self.assertEqual(report['failed'], 0)

    def test_only_established_regular_driver_logs_are_allowed(self):
        report, _ = self.run_phase('ir')
        for item in report['results']:
            if item['name'].startswith('reject-'):
                self.assertEqual(item['driver_logs'], ['logs', 'logs/.rotation-lock', 'logs/log0-last.log'])
                self.assertEqual(item['generated'], [])
        for path in ('unexpected.o', 'logs/unexpected.o', 'logs/log1-last.log', 'logs/unknown'):
            def mutation(process, cwd, prefix):
                if cwd.name.startswith('reject-'):
                    (cwd / path).write_text('unexpected')
            report, _ = self.run_phase('ir', mutation)
            self.assertEqual(report['failed'], 4, path)
        def extra_directory(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                (cwd / 'logs/extra').mkdir()
        report, _ = self.run_phase('ir', extra_directory)
        self.assertEqual(report['failed'], 4)
        for path in ('logs/.rotation-lock', 'logs/log0-last.log'):
            def mutation(process, cwd, prefix):
                if cwd.name.startswith('reject-'):
                    log = cwd / path; log.unlink(); log.symlink_to(next(cwd.glob('*.sts')))
            report, _ = self.run_phase('ir', mutation)
            self.assertEqual(report['failed'], 4, path)
        def directory_symlink(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                logs = cwd / 'logs'
                for path in logs.iterdir():
                    path.unlink()
                logs.rmdir(); logs.symlink_to(cwd, target_is_directory=True)
        report, _ = self.run_phase('ir', directory_symlink)
        self.assertEqual(report['failed'], 4)

    def test_cli_separates_host_helper_and_pinned_native_compilers(self):
        seen = []
        def resolve(value):
            seen.append(value)
            return '/mock/' + value
        with patch.dict(os.environ, {'HOST_CC': 'host-gcc', 'CC': 'pinned-clang'}), \
                patch.object(suite, 'executable', side_effect=resolve), \
                patch.object(suite, 'run_suite', return_value=(dict(results=[], passed=0, failed=0), self.root)) as run:
            self.assertEqual(suite.main(['--phase', 'all', '--compiler', 'candidate']), 0)
        self.assertEqual(seen, ['candidate', 'pinned-clang', 'host-gcc'])
        self.assertEqual([call.args[0] for call in run.call_args_list], ['helper', 'ir', 'native'])
        self.assertTrue(all(call.args[2] == '/mock/pinned-clang' for call in run.call_args_list))
        self.assertTrue(all(call.kwargs['helper_cc'] == '/mock/host-gcc' for call in run.call_args_list))

    def test_helper_compiler_identity_change_fails(self):
        def mutation(process, cwd, prefix):
            if prefix.name == 'run' and cwd.name == 'helper-plain':
                self.helper_cc.write_text('changed helper compiler identity')
        report, _ = self.run_phase('helper', mutation)
        failures = [item for item in report['results'] if item['issues']]
        self.assertEqual([item['name'] for item in failures], ['helper_cc-identity'])

    def test_rejection_artifacts_fail(self):
        def mutation(process, cwd, prefix):
            if cwd.name.startswith('reject-'):
                (cwd / 'stale.o').write_text('not allowed')
        report, _ = self.run_phase('ir', mutation)
        self.assertEqual(report['failed'], 4, report)

    def test_rerun_never_accepts_previous_native_artifact(self):
        receipt = self.ir_receipt()
        first, work1 = self.run_phase('native', receipt=receipt)
        self.assertEqual(first['failed'], 0)
        def mutation(process, cwd, prefix):
            if prefix.name == 'compile' and (cwd / 'program').exists():
                (cwd / 'program').unlink()
        second, work2 = self.run_phase('native', mutation, receipt)
        self.assertNotEqual(work1, work2)
        self.assertEqual(second['failed'], 4)
        self.assertTrue(all('runtime' not in item for item in second['results']))

    def test_stale_or_failed_ir_receipt_blocks_native(self):
        receipt = self.ir_receipt()
        original = json.loads(receipt.read_text())
        for key, value in [('failed', 1), ('passed', 7), ('compiler_sha256', 'stale'),
                           ('source_identities', {}), ('phase', 'native')]:
            record = dict(original); record[key] = value
            receipt.write_text(json.dumps(record))
            before = len(self.calls)
            with self.assertRaises(ValueError):
                self.run_phase('native', receipt=receipt)
            self.assertEqual(len(self.calls), before)
        receipt.write_text(json.dumps(original))
        Path(original['results'][0]['ir_path']).write_text('changed reviewed IR')
        with self.assertRaises(ValueError):
            self.run_phase('native', receipt=receipt)

    def test_duplicate_ir_case_is_not_a_complete_gate(self):
        receipt = self.ir_receipt()
        record = json.loads(receipt.read_text())
        record['results'][1] = record['results'][0]
        receipt.write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            self.run_phase('native', receipt=receipt)

    def test_missing_prerequisites_fail_before_invocation(self):
        with self.assertRaises(ValueError):
            suite.executable(str(self.root / 'absent'))
        with patch.object(suite, 'executable', side_effect=ValueError('missing executable prerequisite')):
            self.assertEqual(suite.main(['--phase', 'ir', '--compiler', 'missing']), 1)
        self.assertEqual(self.calls, [])

    def test_raw_stdout_is_compared_without_normalization(self):
        receipt = self.ir_receipt()
        def mutation(process, cwd, prefix):
            if prefix.name == 'run':
                path = prefix.with_suffix('.stdout')
                path.write_bytes(b'\x1b[0m' + process['stdout'].encode())
                process['stdout_file'] = str(path)
        report, _ = self.run_phase('native', mutation, receipt)
        self.assertEqual(report['failed'], 4)

    def test_helper_requires_marker_and_never_retries_sanitizer(self):
        def mutation(process, cwd, prefix):
            if prefix.name == 'run':
                process.update(stdout='', stderr='LeakSanitizer: unavailable')
        report, _ = self.run_phase('helper', mutation)
        self.assertEqual(report['failed'], 3)
        self.assertEqual(len(self.calls), 6)


if __name__ == '__main__':
    unittest.main()
