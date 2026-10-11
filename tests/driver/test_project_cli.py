"""Mocked runner controls only: passing these does not qualify a compiler."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import project_cli as cli


HELP = 'stasha mock\n\nUsage:\n  stasha [build] <file.sts>\n\nProject mode (sts.sproj in current directory):\n'


def result(code=0, stdout='', stderr='', category=None):
    return dict(returncode=code, category=category or cli.category(code),
                stdout=stdout, stderr=stderr)


class ProjectCliControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / 'mock-compiler'
        self.compiler.write_text('mock identity, never executed')
        self.calls = []
        self.specs = {spec['name']: spec for spec in cli.cases()}

    def mock_runner(self, command, cwd, prefix, timeout):
        self.calls.append((command, cwd, prefix, timeout))
        spec = self.specs[cwd.name]
        if prefix.name == 'execute':
            self.assertEqual(command, [str(cwd / spec['output'])])
            return result(spec['exit'], spec['stdout'])
        self.assertEqual(command, [str(self.compiler), *spec['args']])
        self.assertFalse((cwd / 'a.out').exists())
        self.assertFalse((cwd / cli.PROJECT_OUTPUT).exists())
        if 'early' in spec:
            return result(stdout='stasha mock (ABI 11)\n' if spec['early'] == 'version' else HELP)
        stderr = '\n'.join(cli.expected_compile_messages(spec)) + '\n'
        if 'diagnostic' in spec:
            return result(1, stderr=stderr + 'error: aborting due to 2 previous errors\n')
        output = cwd / spec['output']
        output.write_bytes(b'mocked executable, never executed')
        output.chmod(0o755)
        return result(stderr=stderr)

    def run_all(self, mutation=None):
        def runner(command, cwd, prefix, timeout):
            process = self.mock_runner(command, cwd, prefix, timeout)
            if mutation:
                mutation(process, cwd, prefix)
            return process
        return cli.run_suite(self.compiler, self.root / 'evidence', runner=runner)

    def reject(self, case_name, mutation, stage='compile'):
        def selected(process, cwd, prefix):
            if cwd.name == case_name and prefix.name == stage:
                mutation(process, cwd)
        report, _ = self.run_all(selected)
        selected = next(item for item in report['results'] if item['name'] == case_name)
        self.assertTrue(selected['issues'], selected)
        self.assertGreater(report['failed'], 0)
        return selected

    def test_all_controls_pass_with_isolated_exact_commands(self):
        report, work = self.run_all()
        self.assertEqual((report['passed'], report['failed']), (13, 0), report)
        self.assertEqual(json.loads((work / 'results.json').read_text()), report)
        compiles = [call for call in self.calls if call[2].name == 'compile']
        self.assertEqual(len({call[1] for call in compiles}), 13)
        self.assertEqual(len(self.calls), 19)
        self.assertEqual(report['compiler_sha256_before'], report['compiler_sha256_after'])

    def test_rerun_uses_fresh_directory_and_cannot_reuse_prior_output(self):
        _, old = self.run_all()
        def missing(process, cwd, prefix):
            if prefix.name == 'compile' and cwd.name == 'project-bare':
                (cwd / cli.PROJECT_OUTPUT).unlink()
        report, new = self.run_all(missing)
        self.assertNotEqual(old, new)
        self.assertTrue((old / 'project-bare' / cli.PROJECT_OUTPUT).is_file())
        self.assertEqual(report['failed'], 1)

    def test_preexisting_output_rejected_before_compiler_runs(self):
        original = cli.prepare
        def seed(case, spec):
            original(case, spec)
            if spec['name'] == 'project-bare':
                (case / cli.PROJECT_OUTPUT).write_text('stale')
        with patch.object(cli, 'prepare', seed):
            report, _ = self.run_all()
        record = report['results'][0]
        self.assertIn('expected output already exists before compilation', record['issues'])
        self.assertEqual(record['processes'], [])
        self.assertNotIn('project-bare', [call[1].name for call in self.calls])

    def test_original_bare_help_failure_is_rejected(self):
        def old_behavior(process, cwd):
            process.update(result(1, HELP))
            (cwd / cli.PROJECT_OUTPUT).unlink()
        self.reject('project-bare', old_behavior)

    def test_failed_compiler_with_output_is_not_success(self):
        self.reject('project-bare', lambda p, _: p.update(returncode=1, category='rejection'))

    def test_wrong_output_path_is_rejected(self):
        self.reject('project-bare', lambda p, cwd: (cwd / cli.PROJECT_OUTPUT).rename(cwd / 'a.out'))

    def test_negative_must_be_ordinary_exit_one(self):
        for code, category in [(0, 'success'), (2, 'unexpected-exit'), (-11, 'crash'), (139, 'crash'),
                               (1, 'timeout'), (None, 'launch-error')]:
            with self.subTest(code=code, category=category):
                self.reject('no-project-bare', lambda p, _, code=code, category=category:
                            p.update(returncode=code, category=category))

    def test_expected_diagnostic_is_required(self):
        self.reject('missing-source-bare', lambda p, _: p.update(stderr='different rejection\n'))

    def test_negative_output_or_stale_artifact_is_rejected(self):
        for name in ('no-project-bare', 'missing-source-bare'):
            with self.subTest(name=name):
                self.reject(name, lambda p, cwd: (cwd / 'a.out').write_text('stale executable'))
        self.reject('missing-source-bare', lambda p, cwd: (cwd / cli.PROJECT_OUTPUT).write_text('wrong output'))

    def test_unexpected_compiler_streams_are_rejected(self):
        for name in ('project-bare', 'no-project-bare', 'missing-source-bare'):
            for stream in ('stdout', 'stderr'):
                with self.subTest(name=name, stream=stream):
                    self.reject(name, lambda p, _, stream=stream: p.update({stream: p[stream] + 'unexpected\n'}))

    def test_internal_failure_cannot_count_as_expected_rejection(self):
        for marker in ('LLVM ERROR:', 'AddressSanitizer', 'runtime error:', 'Segmentation fault'):
            with self.subTest(marker=marker):
                self.reject('missing-source-bare', lambda p, _, marker=marker:
                            p.update(stderr=p['stderr'] + marker))

    def test_compile_success_cannot_hide_error_diagnostics(self):
        for marker in ('error: rejected source\n', '(EE) rejected source\n'):
            with self.subTest(marker=marker):
                record = self.reject('project-bare', lambda p, _, marker=marker:
                                     p.update(stderr=p['stderr'] + marker))
                self.assertEqual(len(record['processes']), 1)

    def test_runtime_must_match_exact_stdout_stderr_exit_and_category(self):
        mutations = [dict(stdout=''), dict(stdout=cli.PROJECT_STDOUT + 'extra\n'), dict(stderr='unexpected\n'),
                     dict(returncode=0, category='success'), dict(returncode=-11, category='crash'),
                     dict(category='timeout'), dict(category='launch-error')]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.reject('project-bare', lambda p, _, mutation=mutation: p.update(mutation), 'execute')

    def test_explicit_file_must_not_use_project_source(self):
        self.reject('explicit-file-with-project',
                    lambda p, _: p.update(stdout=cli.PROJECT_STDOUT, returncode=cli.PROJECT_EXIT), 'execute')

    def test_early_flags_must_not_build_or_open_logs(self):
        for name in ('help-with-project', 'short-help-with-project', 'version-with-project'):
            with self.subTest(name=name):
                self.reject(name, lambda p, cwd: (cwd / cli.PROJECT_OUTPUT).write_text('unwanted build'))
        def open_log(process, cwd):
            (cwd / 'logs').mkdir()
            (cwd / 'logs/log0-last.log').write_text('unwanted logger initialization')
        self.reject('help-with-project', open_log)

    def test_early_flags_must_have_expected_output(self):
        self.reject('version-with-project', lambda p, _: p.update(stdout='stasha mock (ABI 11)\nextra\n'))
        self.reject('help-with-project', lambda p, _: p.update(stdout=''))
        self.reject('short-help-with-project', lambda p, _: p.update(stdout=HELP + 'different\n'))
        self.reject('help-with-project', lambda p, _: p.update(stderr='unexpected\n'))

    def test_nonexecutable_and_symlink_output_are_rejected(self):
        self.reject('project-bare', lambda p, cwd: (cwd / cli.PROJECT_OUTPUT).chmod(0o644))
        def symlink(process, cwd):
            output = cwd / cli.PROJECT_OUTPUT
            output.unlink()
            output.symlink_to(cwd / 'src/main.sts')
        self.reject('project-bare', symlink)

    def test_input_mutation_is_rejected(self):
        self.reject('project-bare', lambda p, cwd: (cwd / 'src/main.sts').write_text('changed'))

    def test_compiler_identity_change_is_rejected(self):
        def change(process, cwd, prefix):
            if cwd.name == 'project-bare':
                self.compiler.write_text('changed compiler')
        report, _ = self.run_all(change)
        self.assertNotEqual(report['compiler_sha256_before'], report['compiler_sha256_after'])
        self.assertEqual(report['results'][-1]['name'], 'compiler-identity')
        self.assertEqual(report['failed'], 1)


if __name__ == '__main__':
    unittest.main()
