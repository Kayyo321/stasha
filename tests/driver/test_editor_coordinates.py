"""Mocked runner controls only: passing these does not qualify a compiler."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import editor_coordinates as cli


class EditorCoordinateControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / 'mock-compiler'
        self.compiler.write_text('mock identity, never executed')
        self.specs = {spec['name']: spec for spec in cli.cases()}
        self.calls = []

    def run_all(self, mutation=None):
        def runner(command, cwd, prefix, timeout):
            self.calls.append((command, cwd, prefix, timeout))
            spec = self.specs[cwd.name]
            self.assertEqual(command, [str(self.compiler), *spec['args']])
            self.assertIn(command[1], ('definition', 'complete', 'refs'))
            self.assertEqual(prefix.name, 'query')
            self.assertEqual((cwd / cli.SOURCE_PATH).read_text(), cli.SOURCE)
            self.assertEqual(set(cli.snapshot(cwd)), {cli.SOURCE_PATH, 'untouched.txt'})
            code = 1 if spec.get('rejection') else 0
            process = dict(returncode=code, category=cli.category(code), stderr='',
                           stdout=json.dumps(spec['expected']) + '\n')
            if mutation:
                mutation(process, cwd, prefix)
            return process
        return cli.run_suite(self.compiler, self.root / 'evidence', runner=runner)

    def reject(self, name, mutation):
        def selected(process, cwd, prefix):
            if cwd.name == name:
                mutation(process, cwd, prefix)
        report, _ = self.run_all(selected)
        record = next(item for item in report['results'] if item['name'] == name)
        self.assertTrue(record['issues'], record)
        self.assertGreater(report['failed'], 0)
        return record

    def test_exact_controls_pass_with_only_isolated_editor_queries(self):
        report, work = self.run_all()
        self.assertEqual((report['passed'], report['failed']), (55, 0), report)
        self.assertEqual(len(self.calls), 55)
        self.assertEqual(len({call[1] for call in self.calls}), 55)
        self.assertEqual(json.loads((work / 'results.json').read_text()), report)
        self.assertEqual(report['compiler_sha256_before'], report['compiler_sha256_after'])

    def test_rerun_uses_fresh_directories(self):
        _, first = self.run_all()
        _, second = self.run_all()
        self.assertNotEqual(first, second)

    def test_exact_exit_and_category_required(self):
        for name in ('named-definition', 'line-empty'):
            for code, category in [(2, 'unexpected-exit'), (-11, 'crash'), (139, 'crash'),
                                   (None, 'launch-error'), (0, 'timeout')]:
                with self.subTest(name=name, code=code, category=category):
                    self.reject(name, lambda p, *_: p.update(returncode=code, category=category))
        self.reject('line-empty', lambda p, *_: p.update(returncode=0, category='success'))
        self.reject('named-definition', lambda p, *_: p.update(returncode=1, category='rejection'))

    def test_definition_name_path_kind_and_coordinates_are_exact(self):
        for field, value in [('name', 'caller'), ('path', 'other.sts'), ('kind', 'variable'),
                             ('line', 3), ('col', 8), ('line', True), ('col', 3.0)]:
            with self.subTest(field=field, value=value):
                expected = copy.deepcopy(cli.DEFINITION)
                expected['definition'][field] = value
                self.reject('named-definition', lambda p, *_: p.update(stdout=json.dumps(expected)))
        self.reject('named-definition', lambda p, *_: p.update(stdout='{"definition":null}\n'))
        self.reject('out-of-file', lambda p, *_: p.update(stdout=json.dumps(cli.DEFINITION)))

    def test_old_silent_coercion_is_rejected(self):
        self.reject('line-alphabetic', lambda p, *_:
                    p.update(returncode=0, category='success', stdout='{"definition":null}\n'))
        self.reject('col-suffix', lambda p, *_:
                    p.update(returncode=0, category='success', stdout=json.dumps(cli.DEFINITION)))

    def test_diagnostic_content_and_shape_are_exact(self):
        for expected in [dict(diagnostics=[]), cli.diagnostic('--col'),
                         cli.diagnostic('--line', missing=True),
                         dict(diagnostics=[dict(severity='warning', message='rejected')])]:
            with self.subTest(expected=expected):
                self.reject('line-empty', lambda p, *_: p.update(stdout=json.dumps(expected)))

    def test_stream_and_json_contamination_rejected(self):
        for name in ('named-definition', 'line-empty'):
            for extra in ('\n{}', '\nplain text', '\nLLVM ERROR:', '\nAddressSanitizer'):
                with self.subTest(name=name, extra=extra):
                    self.reject(name, lambda p, *_: p.update(stdout=p['stdout'] + extra))
            self.reject(name, lambda p, *_: p.update(stderr='unexpected\n'))
            self.reject(name, lambda p, *_: p.update(stdout=''))
        self.reject('zero', lambda p, *_:
                    p.update(stdout='{"definition":true,"definition":null}\n'))
        self.reject('zero', lambda p, *_: p.update(stdout='{"definition":NaN}\n'))

    def test_raw_stream_contamination_rejected(self):
        def contaminate(process, cwd, prefix):
            path = prefix.with_suffix('.stdout')
            path.write_bytes(b'\x1b[0m' + process['stdout'].encode())
            process['stdout_file'] = str(path)
        self.reject('named-definition', contaminate)

    def test_completion_and_refs_exact_oracles(self):
        self.reject('complete-valid', lambda p, *_: p.update(stdout='{"completions":[]}\n'))
        self.reject('refs-valid', lambda p, *_: p.update(stdout='{"refs":[]}\n'))
        self.reject('refs-default', lambda p, *_: p.update(stdout=json.dumps(cli.REFS)))

    def test_unexpected_artifacts_rejected_on_success_and_error(self):
        for name in ('named-definition', 'line-empty'):
            for filename in ('a.out', 'a.o', 'a.ll', 'editor.json'):
                with self.subTest(name=name, filename=filename):
                    self.reject(name, lambda p, cwd, _: (cwd / filename).write_text('unexpected'))
            self.reject(name, lambda p, cwd, _: (cwd / 'empty-output-directory').mkdir())

    def test_modified_removed_replaced_or_mode_changed_inputs_rejected(self):
        self.reject('named-definition', lambda p, cwd, _: (cwd / cli.SOURCE_PATH).write_text('changed'))
        self.reject('line-empty', lambda p, cwd, _: (cwd / 'untouched.txt').unlink())
        self.reject('named-definition', lambda p, cwd, _: (cwd / cli.SOURCE_PATH).chmod(0o777))
        def symlink(process, cwd, prefix):
            path = cwd / cli.SOURCE_PATH
            path.unlink()
            path.symlink_to(cwd / 'untouched.txt')
        self.reject('named-definition', symlink)

    def test_compiler_identity_change_rejected(self):
        def change(process, cwd, prefix):
            if cwd.name == 'named-definition':
                self.compiler.write_text('changed')
        report, _ = self.run_all(change)
        record = next(item for item in report['results'] if item['name'] == 'compiler-identity')
        self.assertEqual(record['issues'], ['compiler changed during qualification'])


if __name__ == '__main__':
    unittest.main()
