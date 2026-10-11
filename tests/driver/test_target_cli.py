"""Injected processes and synthetic outputs only; these do not qualify a compiler."""
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import target_cli as cli


STAMP = '2026-10-11 00:00:00'


def elf():
    names = b'\0.text\0.symtab\0.shstrtab\0'
    data = bytearray(64)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', data, 16, 1, 62, 1)
    data += b'\xc3' + bytes(24) + names
    offset = len(data)
    struct.pack_into('<Q', data, 40, offset)
    struct.pack_into('<HHHHHH', data, 52, 64, 0, 0, 64, 4, 3)
    sections = [(0,) * 10,
                (1, 1, 6, 0, 64, 1, 0, 0, 1, 0),
                (7, 2, 0, 0, 65, 24, 0, 0, 8, 24),
                (15, 3, 0, 0, 89, len(names), 0, 0, 1, 0)]
    for section in sections:
        data += struct.pack('<IIQQQQIIQQ', *section)
    return bytes(data)


class TargetCLIControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / 'mock-compiler'
        self.compiler.write_text('injected process identity; never executed')
        self.source = self.root / 'scalar.sts'
        self.source.write_text(cli.SOURCE)
        self.specs = {spec['name']: spec for spec in cli.cases()}
        self.calls = []

    def runner(self, mutation=None):
        def run(command, cwd, prefix, timeout, stdin_mode):
            self.calls.append(command)
            spec = self.specs[cwd.name]
            expected = [str(self.compiler), *[str(cwd.parent / 'input/scalar.sts') if value == '{source}' else cli.LINUX if value == '{native}' else value for value in spec['args']]]
            self.assertEqual(command, expected)
            self.assertIn(command[1], ('obj', 'ir', 'check', 'tokens', 'symbols', 'definition', '--help', '--version'))
            self.assertEqual(stdin_mode, spec.get('stdin', 'closed'))
            self.assertEqual(prefix.name, 'compile')
            kind = spec['kind']
            code = 1 if kind in ('early', 'editor', 'unknown') else 0
            stdout, stderr = '', ''
            if kind == 'editor':
                stdout = json.dumps(cli.diagnostic(spec['message'])) + '\n'
            elif kind == 'early':
                stderr = f"(EE) [{STAMP}] {spec['message']}\nerror: aborting due to 2 previous errors\n"
            elif kind == 'unknown':
                stderr = f"error: unsupported target '{cli.BAD}': no qualified target descriptor\n"
                (cwd / spec['output']).unlink()
            elif kind == 'help':
                stdout = 'stasha MOCK\n\nUsage:\n  --target <triple>\n'
            elif kind == 'version':
                stdout = 'stasha MOCK (ABI 11)\n'
            elif kind == 'obj':
                (cwd / spec['output']).write_bytes(elf())
            elif kind == 'ir':
                triple = spec['triple'] if spec['triple'] not in (None, '{native}') else cli.LINUX
                text = f'target triple = "{triple}"\ndefine i64 @add(i64 %a, i64 %b) {{\n ret i64 0\n}}\n'
                if spec.get('debug'):
                    text += '!DICompileUnit(language: DW_LANG_C)\n; !dbg\n'
                (cwd / spec['output']).write_text(text)
            if kind in ('early', 'unknown', 'obj', 'ir'):
                logs = cwd / 'logs'
                logs.mkdir()
                (logs / '.rotation-lock').write_bytes(b'')
                log = f'=== Log opened at {STAMP} ===\n'
                if kind == 'early':
                    log += stderr.splitlines()[0] + '\n'
                elif kind == 'unknown':
                    log += f"(EE) unsupported target '{cli.BAD}': no qualified target descriptor\n"
                (logs / 'log0-last.log').write_text(log + f'=== Log closed at {STAMP} ===\n')
            process = dict(returncode=code, category=cli.category(code), stdout=stdout, stderr=stderr,
                           command=command, cwd=str(cwd), timeout_seconds=timeout, stdin=stdin_mode, reaped=True)
            if mutation:
                mutation(process, cwd, prefix)
            for stream in ('stdout', 'stderr'):
                path = Path(str(prefix) + '.' + stream)
                path.write_bytes(process.pop('_raw_' + stream, process[stream].encode()))
                process[stream + '_file'] = str(path)
            Path(str(prefix) + '.json').write_text(json.dumps(process))
            return process
        return run

    def run_all(self, mutation=None):
        return cli.run_suite(self.compiler, self.root / 'artifacts', runner=self.runner(mutation))

    def reject(self, name, mutation):
        work = Path(tempfile.mkdtemp(dir=self.root))
        (work / 'input').mkdir()
        source = work / 'input/scalar.sts'
        source.write_text(cli.SOURCE)
        spec = self.specs[name]
        previous = {}
        if spec.get('compare'):
            base = self.specs[spec['compare']]
            previous[base['name']] = cli.run_case(str(self.compiler), work, source, base, 15, self.runner(), {}, cli.sha256(self.compiler), cli.LINUX)
        record = cli.run_case(str(self.compiler), work, source, spec, 15, self.runner(mutation), previous, cli.sha256(self.compiler), cli.LINUX)
        self.assertTrue(record['issues'], record)
        return record

    def test_full_exact_inventory_passes_as_mock_evidence(self):
        report, work = self.run_all()
        self.assertEqual((report['visited'], report['passed'], report['failed']), (82, 82, 0), report)
        self.assertEqual(report['status'], 'PASS')
        self.assertEqual(report['evidence'], 'injected-process controls only')
        self.assertEqual(report['project_loading_precedence'], 'source-only')
        self.assertEqual(len(self.calls), 82)
        self.assertEqual(json.loads((work / 'results.json').read_text()), report)
        self.assertEqual(report['compiler_sha256_before'], report['compiler_sha256_after'])
        self.assertEqual(set(report['excluded_stages'].values()), {'NOT_RUN'})
        self.assertFalse(report['full_k0_readiness'])

    def test_inventory_is_frozen_complete_and_unique(self):
        inventory = cli.cases()
        self.assertEqual(len(inventory), len({spec['name'] for spec in inventory}))
        self.assertEqual(hashlib.sha256(json.dumps(inventory, sort_keys=True).encode()).hexdigest(), cli.INVENTORY_SHA256)
        for changed in (inventory[:-1], list(reversed(inventory)), inventory + [inventory[0]]):
            with self.subTest(count=len(changed)), patch.object(cli, 'cases', return_value=changed):
                with self.assertRaisesRegex(ValueError, 'inventory changed'):
                    self.run_all()
        self.assertEqual(self.calls, [])

    def test_reruns_use_fresh_case_and_evidence_directories(self):
        _, first = self.run_all()
        _, second = self.run_all()
        self.assertNotEqual(first, second)
        self.assertEqual(len(list(first.glob('*-evidence'))), 82)

    def test_exit_signal_timeout_spawn_and_rejection_are_distinct(self):
        for name in ('obj-empty', 'explicit-ir'):
            for code, category in [(2, 'unexpected-exit'), (-11, 'signal'), (139, 'unexpected-exit'),
                                   (None, 'launch-error'), (0, 'timeout'), (True, 'rejection')]:
                with self.subTest(name=name, code=code, category=category):
                    self.reject(name, lambda p, *_: p.update(returncode=code, category=category))
        self.reject('obj-empty', lambda p, *_: p.update(returncode=0, category='success'))
        self.reject('explicit-ir', lambda p, *_: p.update(returncode=1, category='rejection'))

    def test_exact_process_receipt_required(self):
        for key, value in [('command', ['unapproved']), ('cwd', '/wrong'), ('stdin', 'inherited'),
                           ('timeout_seconds', 999), ('reaped', False)]:
            with self.subTest(key=key):
                self.reject('obj-empty', lambda p, *_: p.update({key: value}))

    def test_ordinary_fixed_error_and_logger_summary(self):
        for stderr in ('', 'rejected\n', 'unsupported target\n', cli.OPERAND_ERROR + '\n',
                       f'(EE) [{STAMP}] wrong target message\n',
                       f'(EE) [{STAMP}] {cli.OPERAND_ERROR}\n(EE) extra\n'):
            with self.subTest(stderr=stderr):
                self.reject('obj-empty', lambda p, *_: p.update(stderr=stderr))
        self.reject('obj-empty', lambda p, *_: p.update(stdout='native fallback\n'))

    def test_editor_message_shape_types_and_strict_json(self):
        values = [dict(diagnostics=[]), cli.diagnostic(cli.EQUALS_ERROR)]
        for field, value in [('severity', 'warning'), ('line', True), ('col', 0.0), ('file', 'absent.sts'), ('len', 0)]:
            doc = cli.diagnostic(cli.OPERAND_ERROR)
            doc['diagnostics'][0][field] = value
            values.append(doc)
        for value in values:
            with self.subTest(value=value):
                self.reject('check-empty', lambda p, *_: p.update(stdout=json.dumps(value)))
        for value in ('', '{', '{"diagnostics":[],"diagnostics":[]}', '{"diagnostics":NaN}',
                       json.dumps(cli.diagnostic(cli.OPERAND_ERROR)) + '\n{}',
                       json.dumps(cli.diagnostic(cli.OPERAND_ERROR)) + '\nextra'):
            self.reject('check-empty', lambda p, *_: p.update(stdout=value))
        self.reject('check-empty', lambda p, *_: p.update(stderr='unexpected\n'))

    def test_raw_invalid_utf8_ansi_and_hidden_stderr_rejected(self):
        for raw in (b'\x1b[0m', b'\xff'):
            self.reject('check-empty', lambda p, *_: p.update(_raw_stdout=raw + p['stdout'].encode()))
        self.reject('check-empty', lambda p, *_: p.update(_raw_stderr=b'\x1b[0m'))

    def test_unknown_target_is_separate_invalidation_oracle(self):
        self.reject('unknown-ir', lambda p, cwd, _: (cwd / 'regular.out').write_text('stale'))
        self.reject('unknown-ir', lambda p, *_: p.update(stderr=cli.OPERAND_ERROR))
        self.reject('obj-empty', lambda p, cwd, _: (cwd / 'regular.out').unlink())

    def test_preserves_regular_modes_symlink_targets_and_directories(self):
        changes = [lambda p, cwd, _: (cwd / 'regular.out').write_text('changed'),
                   lambda p, cwd, _: (cwd / 'a.o').chmod(0o777),
                   lambda p, cwd, _: (cwd / 'a.ll').unlink(),
                   lambda p, cwd, _: (cwd / 'target.out').write_text('target changed'),
                   lambda p, cwd, _: (cwd / 'symlink.out').unlink(),
                   lambda p, cwd, _: (cwd / 'directory.out/child').unlink(),
                   lambda p, cwd, _: (cwd / 'directory.out').chmod(0o777)]
        for change in changes:
            self.reject('obj-empty', change)
        def replace_symlink(p, cwd, prefix):
            path = cwd / 'symlink.out'
            path.unlink()
            path.write_text('replacement')
        self.reject('obj-empty', replace_symlink)

    def test_no_unapproved_outputs_sidecars_staging_or_log_lookalikes(self):
        for name in ('extra.o', 'extra.ll', 'a.out', 'extra.a', 'result.o.stasha-abi', 'logs/log1-extra.log'):
            self.reject('obj-empty', lambda p, cwd, _, name=name: (cwd / name).write_text('unexpected'))
        self.reject('explicit-ir', lambda p, cwd, _: (cwd / 'extra.o').write_text('unexpected'))
        self.reject('obj-empty', lambda p, cwd, _: (cwd / '.stasha-stage').mkdir())
        self.reject('check-empty', lambda p, cwd, _: (cwd / 'logs').mkdir())
        self.reject('obj-empty', lambda p, cwd, _: (cwd / 'logs/log0-last.log').write_bytes(b'\x7fELF'))
        self.reject('obj-empty', lambda p, cwd, _: (cwd / 'logs/.rotation-lock').write_text('bad'))
        def log_symlink(p, cwd, prefix):
            path = cwd / 'logs/log0-last.log'
            path.unlink()
            path.symlink_to('../target.out')
        self.reject('obj-empty', log_symlink)

    def test_positive_requires_fresh_nonempty_regular_output(self):
        self.reject('explicit-ir', lambda p, cwd, _: (cwd / 'result.ll').unlink())
        self.reject('explicit-ir', lambda p, cwd, _: (cwd / 'result.ll').write_text(''))
        self.reject('explicit-object', lambda p, cwd, _: (cwd / 'result.o').write_text('sentinel'))
        def output_symlink(p, cwd, prefix):
            path = cwd / 'result.ll'
            path.unlink()
            path.symlink_to('target.out')
        self.reject('explicit-ir', output_symlink)

    def test_elf_header_architecture_and_sections_are_checked(self):
        for offset, data in [(4, b'\x01'), (5, b'\x02'), (16, b'\x02\x00'), (18, b'\xb7\x00'),
                             (52, b'\x00\x00'), (58, b'\x01\x00'), (60, b'\x00\x00')]:
            def mutate(p, cwd, prefix, offset=offset, data=data):
                path = cwd / 'result.o'
                blob = bytearray(path.read_bytes())
                blob[offset:offset + len(data)] = data
                path.write_bytes(blob)
            self.reject('explicit-object', mutate)
        self.reject('explicit-object', lambda p, cwd, _: (cwd / 'result.o').write_bytes(elf()[:64]))

    def test_ir_exact_triples_function_and_debug_controls(self):
        self.reject('explicit-ir', lambda p, cwd, _: (cwd / 'result.ll').write_text('target triple = "wrong"\n'))
        self.reject('repeat-bare-last', lambda p, cwd, _: (cwd / 'result.ll').write_text((cwd / 'result.ll').read_text().replace(cli.BARE, cli.LINUX)))
        self.reject('repeat-linux-last', lambda p, cwd, _: (cwd / 'result.ll').write_text((cwd / 'result.ll').read_text().replace(cli.LINUX, cli.BARE)))
        self.reject('explicit-ir', lambda p, cwd, _: (cwd / 'result.ll').write_text((cwd / 'result.ll').read_text().replace('@add', '@wrong')))
        self.reject('debug-o0', lambda p, cwd, _: (cwd / 'result.ll').write_text((cwd / 'result.ll').read_text().replace('!DICompileUnit(', '!Other(')))
        self.reject('kernel-explicit', lambda p, cwd, _: (cwd / 'result.ll').write_text((cwd / 'result.ll').read_text() + '; changed\n'))

    def test_help_version_precedence_and_streams_are_exact(self):
        self.reject('target-help', lambda p, *_: p.update(stdout='stasha MOCK (ABI 11)\n'))
        self.reject('target-version', lambda p, *_: p.update(stderr='unexpected'))
        self.reject('version-first', lambda p, *_: p.update(stdout='stasha changed (ABI 11)\n'))

    def test_compiler_drift_stops_remaining_inventory(self):
        def change(p, cwd, prefix):
            if cwd.name == 'obj-missing':
                self.compiler.write_text('changed identity')
        report, work = self.run_all(change)
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(report['visited'], 1)
        self.assertIn('not every approved invocation was visited', report['issues'])
        self.assertIn('compiler changed during suite', report['issues'])
        self.assertTrue((work / 'results.json').is_file())

    def test_runner_failure_keeps_terminal_failure_and_no_partial_pass(self):
        with patch.object(cli, 'capture', side_effect=OSError('spawn wrapper failed')):
            report, work = cli.run_suite(self.compiler, self.root / 'artifacts')
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(report['visited'], 1)
        self.assertTrue((work / 'results.json').is_file())

    def test_shared_source_mutation_rejected(self):
        def change(p, cwd, prefix):
            if cwd.name == 'debug-o2':
                (cwd.parent / 'input/scalar.sts').write_text('changed')
        report, _ = self.run_all(change)
        self.assertIn('shared scalar source changed', report['issues'])
        self.assertEqual(report['status'], 'FAIL')

    def test_receipt_write_failure_cannot_return_pass(self):
        original = Path.write_text
        def write(path, *args, **kwargs):
            if path.name == 'results.json':
                raise OSError('receipt denied')
            return original(path, *args, **kwargs)
        with patch.object(Path, 'write_text', write), self.assertRaisesRegex(OSError, 'receipt denied'):
            self.run_all()
        with patch.object(cli, 'run_suite', side_effect=OSError('receipt denied')):
            self.assertEqual(cli.main(['--compiler', str(self.compiler)]), 1)

    def test_nonfinite_or_nonpositive_timeout_rejected_before_invocation(self):
        for timeout in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                cli.run_suite(self.compiler, self.root, timeout, runner=self.runner())
        self.assertEqual(self.calls, [])


@unittest.skipUnless(os.name == 'posix', 'POSIX controlled child process checks')
class CaptureControls(unittest.TestCase):
    def invoke(self, program, stdin='closed', timeout=2):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            return cli.capture([sys.executable, '-c', program], root, root / 'capture', timeout, stdin)

    def test_closed_stdin_is_eof(self):
        process = self.invoke("import sys; assert sys.stdin.read() == ''; print('closed')")
        self.assertEqual((process['category'], process['stdout']), ('success', 'closed\n'))
        self.assertTrue(process['reaped'])

    def test_held_open_pipe_allows_prompt_exit_without_input(self):
        process = self.invoke("import os, stat; assert stat.S_ISFIFO(os.fstat(0).st_mode); print('open')", 'open')
        self.assertEqual((process['category'], process['stdout']), ('success', 'open\n'))
        self.assertTrue(process['reaped'])

    def test_attempt_to_read_held_pipe_times_out_is_killed_and_reaped(self):
        process = self.invoke('import sys; sys.stdin.read()', 'open', 0.1)
        self.assertEqual(process['category'], 'timeout')
        self.assertEqual(process['returncode'], -signal.SIGKILL)
        self.assertTrue(process['reaped'])

    def test_post_spawn_os_error_still_kills_and_reaps(self):
        with patch.object(cli.os, 'killpg', side_effect=OSError('injected group-kill failure')):
            process = self.invoke('import sys; sys.stdin.read()', 'open', 0.1)
        self.assertEqual(process['category'], 'process-error')
        self.assertTrue(process['reaped'])

    def test_signal_is_not_rejection(self):
        process = self.invoke('import os, signal; os.kill(os.getpid(), signal.SIGTERM)')
        self.assertEqual((process['category'], process['returncode']), ('signal', -signal.SIGTERM))

    def test_spawn_failure_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            process = cli.capture([str(root / 'absent')], root, root / 'capture', 1)
            self.assertEqual((process['category'], process['returncode']), ('launch-error', None))
            self.assertIn('error', process)
            self.assertEqual(json.loads((root / 'capture.json').read_text()), process)


if __name__ == '__main__':
    unittest.main()
