#!/usr/bin/env python3
"""Runner regressions use a fake compiler; these do not qualify Stasha."""
from argparse import Namespace
from pathlib import Path
import tempfile
import unittest
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
        if stale:
            case = access.Case('valid', access.program('valid', 'ret 0;'))
        result = access.scalar_runner.run_case(case, 0, args, work)
        return result, work

    def test_explicit_bounded_manifest(self):
        cases = access.cases()
        self.assertEqual(len(cases), 49)
        self.assertEqual(len({c.name for c in cases}), 49)
        self.assertEqual(sum(c.outcome == 'rejection' for c in cases), 34)

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
