#!/usr/bin/env python3
"""Runner regressions use a fake compiler; these do not qualify Stasha."""
from argparse import Namespace
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest import mock
import run_access_contracts as access


class AccessRunnerTests(unittest.TestCase):
    def run_fake(self, mode, stale=False):
        self.addCleanup(lambda: temp.cleanup())
        temp = tempfile.TemporaryDirectory(prefix='memory-oracle-')
        root = Path(temp.name)
        compiler = root / 'fake.py'
        compiler.write_text('''#!/usr/bin/env python3
import os, pathlib, signal, sys, time
mode = "MODE"
source = pathlib.Path(sys.argv[2])
output = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
if mode == 'signal': os.kill(os.getpid(), signal.SIGTERM)
if mode == 'timeout': time.sleep(30)
if mode == 'silent-success': raise SystemExit(0)
if mode.startswith('trap-'):
    reasons = {'trap-bounds': 'bounds', 'trap-null': 'null', 'trap-unknown': 'unknown:6',
               'trap-extra': 'bounds\\nextra'}
    reason = reasons[mode]
    body = 'import sys; sys.stderr.write(' + repr('STASHA_TRAP:' + reason + '\\n') + '); sys.exit(86)'
    output.write_text('#!' + sys.executable + '\\n' + body + '\\n')
    output.chmod(0o700)
    raise SystemExit(0)
if mode == 'artifact': output.write_text('unusable rejected artifact')
print('error: writable pointer required', file=sys.stderr)
print('  --> ' + str(source) + ':4:1', file=sys.stderr)
if mode == 'warning': print("warning: unused variable 'p'", file=sys.stderr)
raise SystemExit(2 if mode == 'wrong-exit' else 1)
'''.replace('MODE', mode))
        compiler.chmod(0o700)
        work = root / 'case'
        if stale:
            work.mkdir()
            (work / 'program').write_text('stale executable')
            (work / 'program').chmod(0o700)
        args = Namespace(compiler=compiler, compiler_arg=[], compile_timeout=0.2,
                         run_timeout=0.2, trap_hook_object=None)
        case = next(c for c in access.cases() if c.name == 'readonly_deref_store')
        if mode.startswith('trap-'):
            case = next(c for c in access.cases() if c.name == 'slice_oob_read')
            args.trap_hook_object = root / 'panic_hook.o'
            args.trap_hook_object.write_bytes(b'mock hook')
            args.trap_hook_pattern = '^STASHA_TRAP:{reason}$'
            args.trap_exit_code = 86
            args.trap_signal = None
            args.require_trap_reasons = True
        if stale:
            case = access.Case('valid', access.program('valid', 'ret 0;'))
        result = access.scalar_runner.run_case(case, 0, args, work)
        return result, work

    def test_explicit_bounded_manifest(self):
        cases = access.cases()
        self.assertEqual(len(cases), 49)
        self.assertEqual(len({c.name for c in cases}), 49)
        self.assertEqual(sum(c.outcome == 'rejection' for c in cases), 34)

    def test_bounds_reasons_and_unsafe_migration(self):
        cases = {case.name: case for case in access.cases()}
        traps = [case for case in cases.values() if case.outcome == 'trap']
        self.assertEqual(len(traps), 7)
        self.assertTrue(all(case.reason == 'bounds' for case in traps))
        self.assertEqual(cases['slice_unsafe'].outcome, 'trap')
        self.assertIn('unsafe { view[1] = 9;', cases['slice_unsafe'].source)
        unchecked = cases['unchecked_slice_store_compound']
        self.assertEqual(unchecked.outcome, 'success')
        self.assertIn('unsafe { view[unchecked: 1]', unchecked.source)
        self.assertIn('a[2] = .{7, 8}', unchecked.source)
        self.assertIn('view = a[0:1]', unchecked.source)
        self.assertIn('if (view[unchecked: 1] != 13)', unchecked.source)

    def test_bounds_trap_requires_exact_hook_reason_and_object_link(self):
        result, _ = self.run_fake('trap-bounds')
        self.assertEqual(result['status'], 'PASS', result)
        self.assertTrue(result['trap_reason_verified'])
        command = result['compile']['command']
        self.assertEqual(Path(command[command.index('-l') + 1]).name, 'panic_hook.o')
        for mode in ('trap-null', 'trap-unknown', 'trap-extra'):
            with self.subTest(mode=mode):
                result, _ = self.run_fake(mode)
                self.assertEqual(result['status'], 'FAIL')
                self.assertFalse(result['trap_reason_verified'])

    def test_prepare_hook_is_strong_reason_checked_and_fail_closed(self):
        args = Namespace(cc='cc', compile_timeout=30)
        work = Path('/unused-memory-selftest')
        selected = [case for case in access.cases() if case.outcome == 'trap']
        built = {'status': 'PASS', 'object': str(work / 'panic-hook/panic_hook.o')}
        with mock.patch.object(access.scalar_runner, 'resolve_cc', return_value=Path('/mock-cc')), \
                mock.patch.object(access.scalar_runner, 'build_trap_hook', return_value=built) as build:
            self.assertEqual(access.prepare_bounds_hook(args, work, selected), built)
            build.assert_called_once_with(work / 'panic-hook', Path('/mock-cc'), 30)
        self.assertEqual(args.trap_hook_object, Path(built['object']))
        self.assertTrue(args.require_trap_reasons)
        self.assertEqual(args.trap_hook_pattern, '^STASHA_TRAP:{reason}$')
        self.assertEqual(args.trap_exit_code, 86)
        self.assertIsNone(args.trap_signal)
        with mock.patch.object(access.scalar_runner, 'resolve_cc', side_effect=ValueError('missing cc')):
            failed = access.prepare_bounds_hook(args, work, selected)
        self.assertEqual(failed['status'], 'FAIL')
        self.assertIsNone(args.trap_hook_object)
        with mock.patch.object(access.scalar_runner, 'resolve_cc') as resolve:
            self.assertIsNone(access.prepare_bounds_hook(args, work, []))
            resolve.assert_not_called()

    def test_runner_entrypoints_use_exact_bounds_hook(self):
        import run_slice_views as views
        for module, name, prefix in ((access, 'slice_oob_read', 'memory-access-'),
                                     (views, 'expand_to_capacity', 'slice-views-')):
            with self.subTest(runner=module.__name__), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                compiler = root / 'fake.py'
                runtime = ('#!' + sys.executable + '\nimport sys\n'
                           'sys.stderr.write("STASHA_TRAP:bounds\\n")\nsys.exit(86)\n')
                compiler.write_text('#!' + sys.executable + '\n'
                    'from pathlib import Path\nimport sys\n'
                    'if sys.argv[1] == "--version": print("mock compiler"); sys.exit(0)\n'
                    'output = Path(sys.argv[sys.argv.index("-o") + 1])\n'
                    'output.write_text(' + repr(runtime) + ')\n'
                    'output.chmod(0o700)\n')
                compiler.chmod(0o700)
                hook = root / 'panic_hook.o'
                hook.write_bytes(b'mock object')
                built = {'status': 'PASS', 'object': str(hook), 'problems': []}
                argv = [module.__file__, '--compiler', str(compiler), '--case', name,
                        '--opt', '0', '--artifacts', str(root)]
                with mock.patch.object(sys, 'argv', argv), \
                        mock.patch.object(access, 'provenance', return_value={}), \
                        mock.patch.object(access.scalar_runner, 'resolve_cc', return_value=Path('/mock-cc')), \
                        mock.patch.object(access.scalar_runner, 'build_trap_hook', return_value=built), \
                        redirect_stdout(io.StringIO()):
                    self.assertEqual(module.main(), 0)
                summary = json.loads(next(root.glob(prefix + '*/summary.json')).read_text())
                self.assertEqual(summary['status'], 'PASS')
                self.assertEqual(summary['hook_build'], built)
                result = summary['results'][0]
                self.assertEqual(result['expected_reason'], 'bounds')
                self.assertTrue(result['trap_reason_verified'])
                command = result['compile']['command']
                self.assertEqual(command[command.index('-l') + 1], str(hook))

    def test_runner_hook_build_failure_never_runs_cases(self):
        import run_slice_views as views
        for module, name, prefix in ((access, 'slice_oob_read', 'memory-access-'),
                                     (views, 'expand_to_capacity', 'slice-views-')):
            with self.subTest(runner=module.__name__), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                compiler = root / 'fake.py'
                compiler.write_bytes(b'not invoked')
                failed = {'status': 'FAIL', 'problems': ['fresh panic hook object missing or empty']}
                argv = [module.__file__, '--compiler', str(compiler), '--case', name,
                        '--opt', '0', '--artifacts', str(root)]
                with mock.patch.object(sys, 'argv', argv), \
                        mock.patch.object(access, 'provenance', return_value={}), \
                        mock.patch.object(access.scalar_runner, 'resolve_cc', return_value=Path('/mock-cc')), \
                        mock.patch.object(access.scalar_runner, 'build_trap_hook', return_value=failed), \
                        mock.patch.object(access.scalar_runner, 'run_case') as run, \
                        mock.patch.object(access.scalar_runner, 'capture') as capture, \
                        redirect_stdout(io.StringIO()):
                    self.assertEqual(module.main(), 1)
                    run.assert_not_called()
                    capture.assert_not_called()
                summary = json.loads(next(root.glob(prefix + '*/summary.json')).read_text())
                self.assertEqual(summary['status'], 'FAIL')
                self.assertEqual(summary['hook_build'], failed)
                self.assertEqual(summary['results'], [])

    def test_unused_rejection_destinations_are_explicit(self):
        cases = {c.name: c for c in access.cases()}
        names = ('readonly_field_conversion', 'readonly_call_conversion',
                 'const_array_address', 'const_field_address',
                 'final_array_address', 'final_field_address')
        for name in names:
            with self.subTest(name=name):
                self.assertEqual(cases[name].outcome, 'rejection')
                self.assertIn('stack i32 *rw _p = ', cases[name].source)

    def test_unexpected_warning_still_fails_rejection(self):
        result, _ = self.run_fake('warning')
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('unexpected compiler warning', result['problems'])
        self.assertNotIn('run', result)

    def test_rejection_requires_exact_exit(self):
        result, _ = self.run_fake('valid')
        self.assertEqual(result['status'], 'PASS')
        result, _ = self.run_fake('wrong-exit')
        self.assertEqual(result['status'], 'FAIL')

    def test_signal_timeout_and_artifact_fail(self):
        for mode in ('signal', 'timeout', 'artifact'):
            with self.subTest(mode=mode):
                result, _ = self.run_fake(mode)
                self.assertEqual(result['status'], 'FAIL')
                self.assertNotIn('run', result)

    def test_rejection_never_executes_accidentally_accepted_program(self):
        result, _ = self.run_fake('silent-success')
        self.assertEqual(result['status'], 'FAIL')
        self.assertNotIn('run', result)

    def test_stale_artifact_cannot_pass(self):
        result, work = self.run_fake('silent-success', stale=True)
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse((work / 'program').exists())
        self.assertNotIn('run', result)


if __name__ == '__main__':
    unittest.main()
