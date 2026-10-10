#!/usr/bin/env python3
"""Oracle and failure-injection tests. These DO NOT execute Stasha/LLVM."""
import hashlib
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace

from generate import Case, cases, write_corpus
from reference import (BY_NAME, TYPES, ScalarError, ScalarTrap, binary, boundaries,
                       canonical_hex, cast, common_type, literal, logical, unary)
from run import build_trap_hook, capture, resolve_cc, run_case


class ReferenceTests(unittest.TestCase):
    def test_literal_contexts_and_defaults(self):
        for typ in TYPES:
            for value in boundaries(typ):
                self.assertEqual(literal(value, typ), (value, typ))
            for value in (typ.minimum - 1, typ.maximum + 1):
                with self.assertRaises(ScalarError):
                    literal(value, typ)
        for value, expected in [(0, 'i32'), (-2147483648, 'i32'), (2147483647, 'i32'),
                                (2147483648, 'i64'), (-2147483649, 'i64'),
                                (-9223372036854775808, 'i64'), (9223372036854775807, 'i64'),
                                (9223372036854775808, 'u64'), (18446744073709551615, 'u64')]:
            self.assertEqual(literal(value)[1].name, expected)
        for value in (-(1 << 63) - 1, 1 << 64):
            with self.assertRaises(ScalarError):
                literal(value)

    def test_exhaustive_eight_bit_arithmetic(self):
        for name in ('i8', 'u8'):
            typ = BY_NAME[name]
            for left in range(typ.minimum, typ.maximum + 1):
                for right in range(typ.minimum, typ.maximum + 1):
                    for op, full in [('+', left + right), ('-', left - right), ('*', left * right)]:
                        # Independent bit encoding, followed by signed decoding.
                        bits = full % 256
                        expected = bits - 256 if typ.signed and bits >= 128 else bits
                        self.assertEqual(binary(op, left, right, typ), expected)
                        self.assertEqual(binary(op + '%', left, right, typ), expected)
                        if typ.fits(full):
                            self.assertEqual(binary(op + '!', left, right, typ), full)
                        else:
                            with self.assertRaisesRegex(ScalarTrap, '^overflow$'):
                                binary(op + '!', left, right, typ)
                    if right and not (typ.signed and left == -128 and right == -1):
                        quotient = binary('/', left, right, typ)
                        remainder = binary('%', left, right, typ)
                        self.assertEqual(left, quotient * right + remainder)
                        self.assertLess(abs(remainder), abs(right))
                        self.assertTrue(remainder == 0 or (remainder < 0) == (left < 0))

    def test_large_width_arithmetic_is_not_host_limited(self):
        randomizer = random.Random(0x535441534841)
        for typ in TYPES:
            for _ in range(100):
                left = randomizer.randint(typ.minimum, typ.maximum)
                right = randomizer.randint(typ.minimum, typ.maximum)
                self.assertEqual(binary('+', left, right, typ) & typ.mask, (left + right) & typ.mask)
                self.assertEqual(binary('*', left, right, typ) & typ.mask, (left * right) & typ.mask)
                if right and not (typ.signed and left == typ.minimum and right == -1):
                    quotient = binary('/', left, right, typ)
                    remainder = binary('%', left, right, typ)
                    self.assertEqual(quotient * right + remainder, left)
            self.assertEqual(unary('-', typ.minimum, typ), typ.wrap(-typ.minimum))
        self.assertEqual(binary('*%', (1 << 64) - 1, (1 << 64) - 1, BY_NAME['u64']), 1)

    def test_division_exception_contract(self):
        for typ in TYPES:
            for op in ('/', '%'):
                with self.assertRaisesRegex(ScalarTrap, '^division-by-zero$'):
                    binary(op, 1, 0, typ)
                if typ.signed:
                    with self.assertRaisesRegex(ScalarTrap, '^division-overflow$'):
                        binary(op, typ.minimum, -1, typ)
        self.assertEqual(binary('/', -7, 3, BY_NAME['i64']), -2)
        self.assertEqual(binary('%', -7, 3, BY_NAME['i64']), -1)
        self.assertEqual(binary('/', 7, -3, BY_NAME['i64']), -2)
        self.assertEqual(binary('%', 7, -3, BY_NAME['i64']), 1)

    def test_shift_range_and_signedness(self):
        for typ in TYPES:
            for op in ('<<', '>>'):
                for count in (-1, typ.width, 256, (1 << 64) - 1):
                    with self.assertRaisesRegex(ScalarTrap, '^invalid-shift$'):
                        binary(op, 1, count, typ)
            self.assertEqual(binary('>>', typ.maximum, typ.width - 1, typ), 0 if typ.signed else 1)
            if typ.signed:
                self.assertEqual(binary('>>', -1, typ.width - 1, typ), -1)

    def test_casts_observe_source_signedness(self):
        self.assertEqual(cast(255, BY_NAME['u8'], BY_NAME['u64']), 255)
        self.assertEqual(cast(-1, BY_NAME['i8'], BY_NAME['u64']), (1 << 64) - 1)
        self.assertEqual(cast(255, BY_NAME['u8'], BY_NAME['i16']), 255)
        self.assertEqual(cast(255, BY_NAME['u16'], BY_NAME['i8']), -1)
        for source in TYPES:
            for destination in TYPES:
                for value in boundaries(source):
                    converted = cast(value, source, destination)
                    self.assertTrue(destination.fits(converted))
                    self.assertEqual(converted & destination.mask, value & destination.mask)
        self.assertEqual(canonical_hex(-1, BY_NAME['i8']), 'ffffffffffffffff')
        self.assertEqual(canonical_hex(255, BY_NAME['u8']), '00000000000000ff')

    def test_mixed_types(self):
        for left in TYPES:
            for right in TYPES:
                if left.signed != right.signed:
                    with self.assertRaises(ScalarError):
                        common_type(left, right)
                else:
                    self.assertEqual(common_type(left, right).width, max(left.width, right.width))

    def test_logical_short_circuit(self):
        def explode():
            raise AssertionError('inactive operand evaluated')
        self.assertFalse(logical('&&', lambda: False, explode))
        self.assertTrue(logical('||', lambda: True, explode))
        called = []
        self.assertTrue(logical('&&', lambda: True, lambda: called.append(1) or True))
        self.assertFalse(logical('||', lambda: False, lambda: called.append(2) or False))
        self.assertEqual(called, [1, 2])


class GeneratorTests(unittest.TestCase):
    def test_inventory_is_deterministic_and_complete(self):
        first, second = cases(), cases()
        self.assertEqual(first, second)
        self.assertEqual(len(first), 577)
        self.assertEqual(len({case.name for case in first}), len(first))
        for typ in TYPES:
            selected = [case for case in first if typ.name in case.tags]
            for outcome in ('success', 'trap', 'rejection'):
                self.assertTrue(any(case.outcome == outcome for case in selected), (typ, outcome))
            for reason in ('overflow', 'division-by-zero', 'invalid-shift'):
                self.assertTrue(any(case.reason == reason for case in selected), (typ, reason))
        for case in first:
            self.assertIn('@[[crash: disable]];', case.source)
            self.assertIn('fn main(void): i32', case.source)
            if case.outcome == 'success':
                self.assertTrue(case.stdout.endswith('SCALAR_OK:' + case.name + '\n'))
            elif case.outcome == 'trap':
                self.assertIn('volatile ', case.source)
            else:
                self.assertTrue(case.diagnostic)

    def test_written_manifest_matches_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            inventory = write_corpus(Path(temp), cases()[:3])
            saved = json.loads((Path(temp) / 'manifest.json').read_text())
            self.assertEqual(saved['cases'], inventory)
            for case in inventory:
                content = (Path(temp) / case['source']).read_bytes()
                self.assertEqual(case['source_sha256'], hashlib.sha256(content).hexdigest())


MOCK_COMPILER = r'''#!/usr/bin/env python3
import os
from pathlib import Path
import signal
import sys
import time
args = sys.argv[1:]
source = Path(args[1])
output = Path(args[args.index('-o') + 1])
mode = next((a.split('=', 1)[1] for a in args if a.startswith('--mode=')), 'success')
if mode == 'compiler-timeout':
    time.sleep(10)
if mode == 'compiler-crash':
    os.kill(os.getpid(), signal.SIGSEGV)
if mode in ('rejection', 'sanitizer', 'generic-error', 'rejected-output'):
    if mode != 'generic-error':
        print('error: literal out of range\n' + source.name + ':3:9 ', file=sys.stderr)
    if mode == 'sanitizer':
        print('AddressSanitizer: detected failure', file=sys.stderr)
    if mode == 'rejected-output':
        output.write_text('stale executable')
    sys.exit(1)
if mode == 'missing-output':
    sys.exit(0)
if mode == 'warning':
    print('warning: unexpected warning', file=sys.stderr)
if mode == 'error-success':
    print('error: rejected despite status zero', file=sys.stderr)
if mode == 'runtime-timeout':
    body = 'import time; time.sleep(10)'
elif mode == 'runtime-sanitizer':
    body = 'import sys; print("AddressSanitizer", file=sys.stderr); print("mock-ok")'
elif mode in ('trap', 'wrong-trap', 'trap-hook', 'wrong-reason'):
    sig = 'SIGSEGV' if mode == 'wrong-trap' else 'SIGILL'
    if mode == 'trap-hook':
        body = 'import sys; print("STASHA_TRAP:overflow", file=sys.stderr); sys.exit(86)'
    elif mode == 'wrong-reason':
        body = 'import sys; print("STASHA_TRAP:division-by-zero", file=sys.stderr); sys.exit(86)'
    else:
        body = 'import os, signal; os.kill(os.getpid(), signal.' + sig + ')'
elif mode == 'wrong-output':
    body = 'print("mock-ok plus stale data")'
elif mode == 'missing-sentinel':
    body = 'pass'
elif mode == 'nonzero-program':
    body = 'import sys; sys.exit(1)'
else:
    body = 'print("mock-ok")'
output.write_text('#!' + sys.executable + '\n' + body + '\n')
output.chmod(0o700)
'''


@unittest.skipUnless(os.name == 'posix', 'POSIX subprocess signal contract')
class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='scalar-oracle-selftest-')
        self.directory = Path(self.temp.name)
        self.compiler = self.directory / 'mock_compiler.py'
        self.compiler.write_text(MOCK_COMPILER)
        self.compiler.chmod(0o700)
        self.args = SimpleNamespace(compiler=self.compiler, compiler_arg=[], compile_timeout=0.8,
                                    run_timeout=0.8, trap_signal='SIGILL', trap_exit_code=None,
                                    trap_hook_pattern=None, require_trap_reasons=False)
        self.case = Case('mock', 'mod mock; fn main(void): i32 { ret 0; }\n', stdout='mock-ok\n')
        self.work = self.directory / 'work'

    def tearDown(self):
        self.temp.cleanup()

    def run_mode(self, mode, case=None):
        self.args.compiler_arg = ['--mode=' + mode]
        return run_case(case or self.case, 2, self.args, self.work)

    def negative_case(self):
        return Case('bad_literal', self.case.source, 'rejection', diagnostic='literal out of range')

    def trap_case(self):
        return Case('overflow', self.case.source, 'trap', reason='overflow')

    def test_success_and_command_syntax(self):
        result = self.run_mode('success')
        self.assertEqual(result['status'], 'PASS', result)
        self.assertIn('-o=2', result['compile']['command'])
        self.assertIn('-o', result['compile']['command'])
        self.assertIn('STS_DUMP_IR_PRE', result['compile']['environment_overrides'])
        self.assertEqual(len(result['binary_sha256']), 64)

    def test_rejection_requires_diagnostic_and_no_binary(self):
        self.assertEqual(self.run_mode('rejection', self.negative_case())['status'], 'PASS')
        for mode in ('generic-error', 'sanitizer', 'compiler-crash', 'rejected-output', 'success'):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_mode(mode, self.negative_case())['status'], 'FAIL')

    def test_stale_output_never_runs(self):
        self.work.mkdir()
        output = self.work / 'program'
        marker = self.directory / 'STALE_WAS_EXECUTED'
        output.write_text('#!' + sys.executable + '\nfrom pathlib import Path\nPath(' + repr(str(marker)) + ').touch()\n')
        output.chmod(0o700)
        result = self.run_mode('missing-output')
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(marker.exists())
        self.assertFalse(output.exists())
        self.assertNotIn('run', result)

    def test_all_failure_modes_fail_closed(self):
        for mode in ('compiler-timeout', 'compiler-crash', 'missing-output', 'warning', 'error-success',
                     'runtime-timeout', 'runtime-sanitizer', 'wrong-output', 'missing-sentinel', 'nonzero-program'):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_mode(mode)['status'], 'FAIL', mode)

    def test_signal_trap_requires_exact_signal(self):
        result = self.run_mode('trap', self.trap_case())
        self.assertEqual(result['status'], 'PASS', result)
        self.assertFalse(result['trap_reason_verified'])
        for mode in ('wrong-trap', 'nonzero-program', 'success', 'runtime-timeout'):
            self.assertEqual(self.run_mode(mode, self.trap_case())['status'], 'FAIL')

    def test_hook_build_missing_output_fails_closed(self):
        fake_cc = self.directory / 'mock_cc.py'
        fake_cc.write_text('#!' + sys.executable + '\nimport sys\n'
                           'if "--version" in sys.argv: print("Mock CC version 1")\n')
        fake_cc.chmod(0o700)
        work = self.directory / 'hook-build'
        work.mkdir()
        (work / 'panic_hook.o').write_bytes(b'stale object')
        result = build_trap_hook(work, fake_cc, 1)
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse((work / 'panic_hook.o').exists())
        self.assertIn('fresh panic hook object missing or empty', result['problems'])
        with self.assertRaises(ValueError):
            resolve_cc(str(self.directory / 'absent-cc'))

    def test_hook_object_linked_only_for_trap_cases(self):
        self.args.trap_hook_object = self.directory / 'panic_hook.o'
        self.args.trap_hook_object.write_bytes(b'mock object')
        self.args.trap_hook_pattern = '^STASHA_TRAP:{reason}$'
        self.args.trap_exit_code = 86
        self.args.require_trap_reasons = True
        result = self.run_mode('trap-hook', self.trap_case())
        self.assertEqual(result['status'], 'PASS', result)
        command = result['compile']['command']
        self.assertEqual(command[command.index('-l') + 1], str(self.args.trap_hook_object))
        positive = self.run_mode('success')
        self.assertEqual(positive['status'], 'PASS', positive)
        self.assertNotIn('-l', positive['compile']['command'])
        rejected = self.run_mode('rejection', self.negative_case())
        self.assertEqual(rejected['status'], 'PASS', rejected)
        self.assertNotIn('-l', rejected['compile']['command'])

    def test_full_qualification_rejects_signal_only(self):
        self.args.require_trap_reasons = True
        self.assertEqual(self.run_mode('trap', self.trap_case())['status'], 'FAIL')

    def test_hook_requires_exact_expected_reason(self):
        self.args.trap_hook_pattern = '^STASHA_TRAP:{reason}$'
        self.args.trap_exit_code = 86
        self.args.require_trap_reasons = True
        result = self.run_mode('trap-hook', self.trap_case())
        self.assertEqual(result['status'], 'PASS', result)
        self.assertTrue(result['trap_reason_verified'])
        self.assertEqual(self.run_mode('wrong-reason', self.trap_case())['status'], 'FAIL')

    def test_launch_failure_is_not_a_diagnostic(self):
        self.args.compiler = self.directory / 'missing-compiler'
        result = self.run_mode('success', self.negative_case())
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['compile']['category'], 'launch-error')

    def test_cli_writes_completed_machine_readable_summary(self):
        script = Path(__file__).with_name('run.py')
        # Select a generated rejection that the mock can faithfully reject.
        completed = subprocess.run([sys.executable, str(script), '--compiler', str(self.compiler),
            '--filter', '^reject_literal_i8_above_local$', '--opt', '0,1,2,3',
            '--artifacts', str(self.directory), '--compiler-arg=--mode=rejection'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        report = json.loads(completed.stdout.strip().splitlines()[-1])
        summary = json.loads(Path(report['summary']).read_text())
        self.assertEqual(summary['status'], 'PASS')
        self.assertEqual(summary['passed'], 4)
        self.assertEqual(summary['failed'], 0)
        self.assertFalse(summary['full_p1_trap_reason_qualification'])
        self.assertEqual({item['optimization'] for item in summary['results']}, {0, 1, 2, 3})
        for result in summary['results']:
            detail = json.loads(Path(result['result_file']).read_text())
            self.assertEqual(detail['status'], 'PASS')
            self.assertNotIn('run', detail)
            self.assertIn('source_sha256', detail)

    def test_cli_no_empty_or_unknown_optimization_passes(self):
        script = Path(__file__).with_name('run.py')
        for arguments in (['--list', '--filter', '^never_match$'], ['--list', '--opt', '4'],
                          ['--list', '--opt', '0,0'], ['--list', '--run-timeout', 'nan'],
                          ['--compiler', str(self.directory / 'missing')]):
            completed = subprocess.run([sys.executable, str(script)] + arguments,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertNotEqual(completed.returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
