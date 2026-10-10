#!/usr/bin/env python3
"""Failure injection for the cleanup oracle; mock-only, not compiler evidence."""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace

import run

MOCK = r'''#!PYTHON
import os, pathlib, signal, sys, time
source = pathlib.Path(sys.argv[2])
output = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
mode = sys.argv[-1]
if mode == 'compile_timeout': time.sleep(30)
if mode == 'compile_signal': os.kill(os.getpid(), signal.SIGTERM)
if mode == 'missing': sys.exit(0)
if mode == 'compile_fail': sys.exit(1)
if mode in ('reject', 'reject_noloc', 'reject_success', 'reject_crash', 'reject_artifact', 'reject_otherexit'):
    print('error: return inside defer', file=sys.stderr)
    if mode != 'reject_noloc': print(f'{source.name}:3:5', file=sys.stderr)
    if mode == 'reject_crash': os.kill(os.getpid(), signal.SIGTERM)
    if mode == 'reject_artifact': output.write_text('not runnable')
    sys.exit(0 if mode == 'reject_success' else 2 if mode == 'reject_otherexit' else 1)
if mode == 'warning': print('warning: unexpected warning', file=sys.stderr)
if mode == 'false_error': print('error: compilation failed', file=sys.stderr)
if mode == 'verifier': print('LLVM IR verification failed', file=sys.stderr)
if mode == 'sanitizer': print('AddressSanitizer: heap-use-after-free', file=sys.stderr)
body = "import os, signal, sys, time\n"
if mode == 'runtime_timeout': body += "time.sleep(30)\n"
if mode == 'runtime_signal': body += "os.kill(os.getpid(), signal.SIGTERM)\n"
if mode == 'runtime_fail': body += "sys.exit(4)\n"
if mode == 'runtime_stderr': body += "print('unexpected', file=sys.stderr)\n"
if mode == 'wrong_order': body += "print('BA:DONE')\n"
elif mode == 'duplicate': body += "print('ABB:DONE')\n"
elif mode == 'partial': body += "print('AB:')\n"
elif mode == 'ansi_extra': body += "print('\\x1b[31mAB:DONE\\x1b[0m')\n"
else: body += "print('AB:DONE')\n"
output.write_text('#!' + sys.executable + '\n' + body)
output.chmod(0o755)
if mode == 'failed_with_binary': sys.exit(1)
'''


@unittest.skipUnless(os.name == 'posix', 'POSIX executable mock fixtures')
class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cleanup-oracle-')
        self.root = Path(self.temp.name)
        self.saved_here = run.HERE
        run.HERE = self.root
        (self.root / 'case.sts').write_text('mod fixture; fn main(void): i32 { ret 0; }\n')
        self.compiler = self.root / 'compiler'
        self.compiler.write_text(MOCK.replace('PYTHON', sys.executable, 1))
        self.compiler.chmod(0o755)
        self.case = dict(name='fixture', source='case.sts', outcome='success', stdout='AB:DONE\n')

    def tearDown(self):
        run.HERE = self.saved_here
        self.temp.cleanup()

    def execute(self, mode, case=None, work=None):
        args = SimpleNamespace(compiler=self.compiler, compiler_arg=[mode], compile_timeout=1,
                               run_timeout=1)
        if 'timeout' in mode:
            args.compile_timeout = args.run_timeout = 0.1
        return run.run_case(case or self.case, 0, args, work or self.root / ('run-' + mode))

    def test_pass_requires_fresh_exact_trace(self):
        result = self.execute('good')
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['run']['stdout'], 'AB:DONE\n')
        self.assertEqual(len(result['binary_sha256']), 64)

    def test_compile_failures_never_execute(self):
        for mode in ('missing', 'compile_fail', 'compile_signal', 'compile_timeout', 'warning',
                     'false_error', 'verifier', 'sanitizer', 'failed_with_binary'):
            with self.subTest(mode=mode):
                result = self.execute(mode)
                self.assertEqual(result['status'], 'FAIL', result)
                self.assertNotIn('run', result)

    def test_missing_output_removes_stale_binary(self):
        work = self.root / 'stale'
        work.mkdir()
        marker = self.root / 'executed-stale'
        output = work / 'program'
        output.write_text(f'#!{sys.executable}\nopen({str(marker)!r}, "w").write("bad")\n')
        output.chmod(0o755)
        result = self.execute('missing', work=work)
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(output.exists())
        self.assertFalse(marker.exists())

    def test_rejects_changed_or_duplicate_or_partial_trace(self):
        for mode in ('wrong_order', 'duplicate', 'partial', 'ansi_extra'):
            with self.subTest(mode=mode):
                result = self.execute(mode)
                self.assertEqual(result['status'], 'FAIL', result)
                self.assertIn('exact cleanup trace mismatch', result['problems'])

    def test_runtime_failure_signal_timeout_and_stderr(self):
        for mode in ('runtime_timeout', 'runtime_signal', 'runtime_fail', 'runtime_stderr'):
            with self.subTest(mode=mode):
                result = self.execute(mode)
                self.assertEqual(result['status'], 'FAIL', result)
                self.assertIn('run', result)

    def test_strict_rejection_contract(self):
        case = dict(self.case, outcome='rejection', diagnostic='return inside defer')
        self.assertEqual(self.execute('reject', case)['status'], 'PASS')
        for mode in ('reject_noloc', 'reject_success', 'reject_crash', 'reject_artifact', 'reject_otherexit'):
            with self.subTest(mode=mode):
                self.assertEqual(self.execute(mode, case)['status'], 'FAIL')
        case['diagnostic'] = 'unrelated error'
        self.assertEqual(self.execute('reject', case, self.root / 'wrong-diagnostic')['status'], 'FAIL')

    def test_missing_compiler_is_launch_error(self):
        args = SimpleNamespace(compiler=self.root / 'absent', compiler_arg=[], compile_timeout=1, run_timeout=1)
        result = run.run_case(self.case, 0, args, self.root / 'no-compiler')
        self.assertEqual(result['compile']['category'], 'launch-error')
        self.assertEqual(result['status'], 'FAIL')

    def test_artifacts_have_compile_and_run_provenance(self):
        result = self.execute('good')
        saved = json.loads((self.root / 'run-good/result.json').read_text())
        self.assertEqual(saved, result)
        self.assertEqual(saved['compile']['cwd'], str(self.root / 'run-good'))
        self.assertEqual(saved['compile']['command'][1], 'build')
        self.assertIn('-o=0', saved['compile']['command'])

    def test_manifest_rejects_path_escape_and_duplicate_name(self):
        manifest = self.root / 'cases.json'
        for data in ([self.case, self.case], [dict(self.case, source='../case.sts')],
                     [dict(self.case, name='../x')], [dict(self.case, stdout=None)]):
            with self.subTest(data=data):
                manifest.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    run.load_cases(manifest)

    def test_timeouts_must_be_positive_and_finite(self):
        for value in ('0', '-1', 'inf', 'nan'):
            with self.subTest(value=value):
                with self.assertRaises(Exception):
                    run.positive_timeout(value)
        self.assertEqual(run.positive_timeout('1.5'), 1.5)



class ZoneProbeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler unavailable')
    def test_real_runtime_wrapper_detects_leak_and_duplicate_release(self):
        # This is actual native C execution, but never Stasha codegen evidence.
        with tempfile.TemporaryDirectory(prefix='cleanup-zone-probe-') as temp:
            work = Path(temp)
            source = work / 'main.c'
            source.write_text("""#include <stddef.h>
extern void *__zone_alloc(void **, size_t);
extern void __zone_free(void **);
int main(int argc, char **argv) {
    (void)argv;
    void *zone = 0;
    int *value = __zone_alloc(&zone, sizeof(*value));
    *value = 73;
    if (*value != 73) return 1;
    if (argc != 2) __zone_free(&zone);
    if (argc == 3) __zone_free(&zone);
    return 0;
}
""")
            binary = work / 'probe'
            built = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    str(run.HERE / 'zone_probe.c'), str(source), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=20)
            self.assertEqual(built.returncode, 0, built.stderr)
            valid = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertEqual(valid.stdout, 'zone.release 1\n')
            self.assertEqual(valid.stderr, '')
            for args, diagnostic in [(['leak'], 'zone not released'),
                                     (['double', 'release'], 'duplicate or empty zone release')]:
                failed = subprocess.run([str(binary)] + args, capture_output=True, text=True, timeout=2)
                self.assertEqual(failed.returncode, 91)
                self.assertIn(diagnostic, failed.stderr)

if __name__ == '__main__':
    unittest.main()
