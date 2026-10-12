"""Pure controls: synthetic artifacts and mocked capture never execute Stasha/images."""
import copy
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest import mock

spec = importlib.util.spec_from_file_location('lifecycle_runner', Path(__file__).with_name('run_lifecycle.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def model(session, binary, opt, run_id):
    """A self-consistent wire model, not a report learned from native execution."""
    data = dict(schema=4, profile=runner.support.PROFILE, runtime_abi=4,
        allocation_metric_version=2, line_metric_version=2, target='x86_64-unknown-linux-gnu',
        compiler_sha256=session.compiler_hash, runtime_sha256=session.runtime_hash,
        binary_sha256=runner.support.sha(binary), run=run_id, report_valid=True,
        complete=1, reason=0, suite_endpoint=1, compile_link_success=True, manifest_valid=True,
        overall_status=0, child=dict(launched=1, waited=1, exited=1, exit_code=0, signal=0, launch_errno=0, wait_errno=0),
        assertions_passed=4, assertions_failed=0, tests_started=2, tests_completed=2,
        live_allocations=0, tracked_outstanding_bytes=0, allocation_domains=copy.deepcopy(runner.ZERO_DOMAINS),
        allocations=[], enabled=3, warn_memleak=False, max_leaked_bytes=0, min_test_coverage=90,
        covered_lines=20, coverage_metric='pass', allocation_metric='pass',
        invocation_directory_bytes_hex=os.fsencode(runner.ROOT).hex(),
        original_root_spelling_bytes_hex=os.fsencode(runner.FIXTURES / 'success.sts').hex())
    data['config_sha256'] = runner.support.config_digest(session.compiler_hash, session.runtime_hash, opt,
        enabled=3, warn=0, maximum=0, minimum=90)[0]
    data['files'] = []
    for index, name in enumerate(('success.sts', 'lifecycle.sts')):
        path = runner.FIXTURES / name
        count = 1 + path.read_bytes().count(b'\n')
        lines = runner.LEAF_LINES if index else []
        hits = runner.LEAF_HITS if index else []
        masks = [2] * count
        if index == 0:
            masks[2:8] = [8] * 6
            masks[8:18] = [4] * 10
        data['files'].append(dict(id=index, role=index, physical_line_count=count,
            source_sha256=runner.support.sha(path), source_bytes=path.stat().st_size,
            lookup_path_bytes_hex=os.fsencode(path).hex(), canonical_path_bytes_hex=os.fsencode(path).hex(),
            requested_module_bytes_hex=b'lifecycle'.hex() if index else None,
            declared_module_bytes_hex=(b'lifecycle' if index else b'lifecycle_success').hex(),
            exclusions=masks, lines=lines, hit_lines=hits, eligible_lines=len(lines), covered_lines=len(hits)))
    data['edges'] = [dict(parent_file_id=0, child_file_id=1, line=2, col=1, ordinal=0,
        requested_module_bytes_hex=b'lifecycle'.hex(), lookup_path_bytes_hex=os.fsencode(runner.FIXTURES / 'lifecycle.sts').hex())]
    data['declarations'] = []
    for index, (name, file_id, line, end_line, selected, kind) in enumerate((
            ('main', 0, 3, 8, 0, 1), ('', 0, 9, 13, 1, 58), ('', 0, 14, 18, 1, 58),
            ('run', 1, 6, 34, 1, 1))):
        data['declarations'].append(dict(id=index, file_id=file_id, ordinal_in_file=index if not file_id else 0,
            kind=kind, line=line, col=1, end_line=end_line, end_col=1, selected=selected, source_linkage=0,
            module_name_bytes_hex=(b'lifecycle' if file_id else b'lifecycle_success').hex(), name_bytes_hex=name.encode().hex()))
    data['tests'] = [dict(id=i, decl_id=i+1, file_id=0, line=line, col=1, name_bytes_hex=name.hex())
        for i, (line, name) in enumerate(((9, b'success lifecycle'), (14, b'error lifecycle')))]
    data['anchors'] = [dict(id=0, decl_id=1, file_id=0, line=10, col=1, end_line=10, end_col=1,
        kind=1, exclusion_reason=2, coverage_id=None)]
    data['sites'] = [dict(id=i, decl_id=3, file_id=1, line=line, col=1, end_line=line, end_col=1,
        kind=kind, static_test_id=None, slice_element_kind=0, slice_abi_stride=0, slice_count_arity=0,
        slice_initializer_item_count=0) for i, (line, kind) in enumerate(((9, 1), (16, 3)))]
    data['coverage'] = [dict(id=i, file_id=1, line=line, hit=line in runner.LEAF_HITS)
                        for i, line in enumerate(runner.LEAF_LINES)]
    rehash(data)
    return data


def rehash(data):
    data['input_graph_sha256'] = runner.support.recompute_graph(data)
    data['map_sha256'] = runner.support.recompute_map(data)


def raw_report(data):
    return ('STASHA_OBSERVATION 4\nprofile ' + runner.support.PROFILE + '\nruntime_abi 4\nmetric_versions 2 2\n'
        + ''.join(f'{label} {data[key]}\n' for label, key in (
            ('run', 'run'), ('input_graph', 'input_graph_sha256'), ('map', 'map_sha256'),
            ('config', 'config_sha256'), ('compiler', 'compiler_sha256')))
        + 'enabled 3\ninventory 2 22 2\nstate 1 0 1\nassertions 4 0\ntests 2 2\ntest 0 2\ntest 1 2\n'
        + 'hits dfff2f\nallocations 0 0\nEND\n').encode()


class LifecycleRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = self.root / 'stasha'
        self.compiler.write_bytes(b'PURE MOCK: not an executable program\n')
        self.compiler.chmod(0o700)
        (self.root / 'observation_runtime.a').write_bytes(b'PURE MOCK runtime\n')
        self.evidence = []
        self.addCleanup(self.clean_evidence)

    def clean_evidence(self):
        for path in self.evidence:
            for file in path.iterdir():
                file.unlink()
            path.rmdir()

    def session(self):
        return runner.LifecycleRun(self.compiler, self.root / 'artifacts')

    def fake_capture(self, command, cwd, prefix, timeout, extra_env=None):
        command = list(map(str, command))
        stdout, stderr, code, category = b'', b'', 0, 'success'
        if len(command) == 1:
            name = 'error' if Path(command[0]).name.startswith('error-') else 'success'
            code, category, stdout = runner.ORDINARY[name]
        else:
            stderr = b'[2000-01-01 00:00:00] generating code\n[2000-01-01 00:00:00] linking\n'
            binary = Path(command[command.index('-o') + 1])
            binary.write_bytes(b'PURE MOCK IMAGE\n')
            binary.chmod(0o700)
            observed = command[1] == 'test'
            for key in ('STS_DUMP_IR_PRE', 'STS_DUMP_IR'):
                Path(extra_env[key]).write_bytes(b'; PURE MOCK IR\n' + (b'@__stasha_obs_init_v4\n' if observed else b''))
            if observed:
                opt = int(next(x[3:] for x in command if x.startswith('-o=')))
                directory = Path('/tmp') / ('stasha-observation-' + uuid.uuid4().hex)
                directory.mkdir(mode=0o700)
                self.evidence.append(directory)
                identity = type('Identity', (), dict(compiler_hash=runner.support.sha(self.compiler),
                    runtime_hash=runner.support.sha(self.compiler.parent / 'observation_runtime.a')))()
                data = model(identity, binary, opt, f'{opt+1:064x}')
                (directory / 'driver.json').write_text(json.dumps(data))
                (directory / 'report').write_bytes(raw_report(data))
                stdout = runner.OBSERVED_STDOUT
                stderr += b'observation: private evidence ' + os.fsencode(directory) + b'\n'
        result = dict(command=command, cwd=str(cwd), timeout_seconds=timeout, returncode=code, category=category,
            stdout=stdout.decode(), stderr=stderr.decode(), environment_overrides=extra_env or {},
            stdout_file=str(prefix) + '.stdout', stderr_file=str(prefix) + '.stderr')
        Path(str(prefix) + '.stdout').write_bytes(stdout)
        Path(str(prefix) + '.stderr').write_bytes(stderr)
        Path(str(prefix) + '.json').write_text(json.dumps(result))
        return result

    def invoke(self, callback=None):
        with mock.patch.object(runner, 'capture', side_effect=callback or self.fake_capture), redirect_stdout(io.StringIO()):
            code = runner.run(self.compiler, self.root / 'artifacts')
        latest = Path((self.root / 'artifacts/LATEST').read_text().strip())
        return code, json.loads((latest / 'result.json').read_text()), latest

    def test_frozen_fixture_hashes_and_literal_oracles(self):
        self.assertEqual({name: runner.support.sha(runner.FIXTURES / name) for name in runner.FIXTURE_HASHES}, runner.FIXTURE_HASHES)
        self.assertEqual(runner.LEVELS, [0, 1, 2, 3])
        self.assertEqual((len(runner.LEAF_LINES), len(runner.LEAF_HITS)), (22, 20))
        self.assertEqual(set(runner.LEAF_LINES) - set(runner.LEAF_HITS), {11, 31})
        self.assertEqual(runner.FLAGS, ['--max-leaked-bytes', '0', '--min-test-coverage', '90'])
        self.assertEqual(runner.ORDINARY['error'][:2], (23, 'unexpected-exit'))
        self.assertNotIn(b'MAIN ', runner.OBSERVED_STDOUT)

    def test_exact_twenty_commands_and_once_only_execution_order(self):
        code, result, latest = self.invoke()
        self.assertEqual(code, 0, result)
        self.assertEqual([r['name'] for r in result['commands']], runner.expected_commands())
        self.assertEqual([c['name'] for c in result['cases']], runner.expected_cases())
        self.assertEqual((result['ordinary_executions'], result['observed_driver_invocations']), (8, 4))
        commands = [r['result'] for r in result['commands']]
        self.assertEqual([r['timeout_seconds'] for r in commands], [120, 10] * 8 + [120] * 4)
        self.assertEqual(sum(len(r['command']) == 1 for r in commands), 8)
        self.assertEqual(sum(r['command'][1:2] == ['test'] for r in commands), 4)
        self.assertTrue((latest / 'artifact-identities.json').is_file())

    def test_abnormal_status_and_launch_failures_rejected_despite_good_trace(self):
        for code, category in ((-11, 'crash'), (139, 'crash'), (0, 'timeout'), (None, 'launch-error'),
                               (1, 'rejection'), (23, 'success'), (0, 'unexpected-exit')):
            with self.subTest(code=code, category=category):
                self.assertRaises(AssertionError, runner.validate_status, dict(returncode=code, category=category), 0, 'success')

    def test_raw_trace_not_ansi_stripped_capture_text(self):
        def corrupt(*args, **kwargs):
            result = self.fake_capture(*args, **kwargs)
            if len(args[0]) == 1:
                Path(str(args[2]) + '.stdout').write_bytes(b'\x1b[31m' + runner.ORDINARY['success'][2])
            return result
        code, result, _ = self.invoke(corrupt)
        self.assertEqual(code, 1)
        self.assertIn('wrong ordinary trace', result['error'])
        self.assertEqual(len(result['commands']), 2)

    def test_compile_and_observed_abnormal_status_stop_before_pass(self):
        for stage in ('build', 'test'):
            for code, category in ((-11, 'crash'), (0, 'timeout'), (None, 'launch-error'), (1, 'rejection')):
                def corrupt(*args, **kwargs):
                    result = self.fake_capture(*args, **kwargs)
                    if len(args[0]) > 1 and args[0][1] == stage:
                        result.update(returncode=code, category=category)
                        Path(str(args[2]) + '.json').write_text(json.dumps(result))
                    return result
                with self.subTest(stage=stage, code=code, category=category):
                    outcome, result, _ = self.invoke(corrupt)
                    self.assertEqual(outcome, 1)
                    self.assertEqual(result['status'], 'fail')
                    self.assertEqual(len(result['commands']), 1 if stage == 'build' else 17)

    def test_build_time_main_stdout_is_rejected_before_ordinary_execution(self):
        def premature(*args, **kwargs):
            result = self.fake_capture(*args, **kwargs)
            Path(str(args[2]) + '.stdout').write_bytes(runner.ORDINARY['success'][2])
            return result
        code, result, _ = self.invoke(premature)
        self.assertEqual(code, 1)
        self.assertIn('premature execution', result['error'])
        self.assertEqual(len(result['commands']), 1)

    def test_exit_zero_compiler_diagnostics_rejected_in_both_modes(self):
        for stage in ('build', 'test'):
            for diagnostic in (b'error: broken\n', b'error[undefined]: broken\n',
                    b'(EE) [2000-01-01 00:00:00] broken\n', b'warning: unexpected\n',
                    b'\x1b[31mwarning[unused]: unexpected\x1b[0m\n', b'(!!) unexpected\n', b'1 warning emitted\n'):
                def corrupt(*args, **kwargs):
                    result = self.fake_capture(*args, **kwargs)
                    if len(args[0]) > 1 and args[0][1] == stage:
                        path = Path(str(args[2]) + '.stderr')
                        path.write_bytes(path.read_bytes() + diagnostic)
                    return result
                with self.subTest(stage=stage, diagnostic=diagnostic):
                    code, result, _ = self.invoke(corrupt)
                    self.assertEqual(code, 1, result)
                    self.assertIn('unexpected compiler error/warning diagnostic', result['error'])
                    self.assertEqual(len(result['commands']), 1 if stage == 'build' else 17)

    def test_normal_compiler_and_observer_logs_are_accepted(self):
        normal = b"[2000-01-01 00:00:00] compiled '/path/error.sts' -> '/path/image'\n"
        observed = normal + (b'observation: child exit 0\n'
            b'observation: tests 2/2 returned; assertions 4 passed, 0 failed; complete (reason 0)\n'
            b'observation: tracked outstanding requested bytes 0 in 0 allocations; maximum 0 (pass)\n'
            b'observation: executable statement/control-line coverage 20/22; minimum 90% (pass)\n'
            b'observation: uncovered source lines 1:11 1:31\n'
            b'observation: private evidence /tmp/stasha-observation-ABC123\n')
        runner.validate_compiler_streams(b'', normal, observed=False)
        runner.validate_compiler_streams(runner.OBSERVED_STDOUT, observed, observed=True)

    def test_wrong_status_and_stderr_block_observation(self):
        for mutation in ('status', 'stderr'):
            def corrupt(*args, **kwargs):
                result = self.fake_capture(*args, **kwargs)
                if len(args[0]) == 1:
                    if mutation == 'status':
                        result.update(returncode=23, category='unexpected-exit')
                        Path(str(args[2]) + '.json').write_text(json.dumps(result))
                    else:
                        Path(str(args[2]) + '.stderr').write_bytes(b'unexpected\n')
                return result
            with self.subTest(mutation=mutation):
                code, result, _ = self.invoke(corrupt)
                self.assertEqual(code, 1)
                self.assertEqual(len(result['commands']), 2)

    def test_missing_image_ir_or_report_cannot_pass(self):
        for mutation in ('image', 'pre', 'post', 'report', 'driver.json'):
            def corrupt(*args, **kwargs):
                result = self.fake_capture(*args, **kwargs)
                command = list(map(str, args[0]))
                if len(command) > 1:
                    if mutation == 'image':
                        Path(command[command.index('-o') + 1]).unlink()
                    elif mutation in ('pre', 'post'):
                        Path(kwargs['extra_env']['STS_DUMP_IR_PRE' if mutation == 'pre' else 'STS_DUMP_IR']).unlink()
                    elif command[1] == 'test':
                        (self.evidence[-1] / mutation).unlink()
                return result
            with self.subTest(mutation=mutation):
                code, result, _ = self.invoke(corrupt)
                self.assertEqual(code, 1, result)

    def test_missing_incomplete_malformed_and_nonzero_report_rejected(self):
        for mutation in ('empty', 'incomplete', 'duplicate-json', 'live', 'bytes', 'domain', 'child', 'bitmap', 'main', 'site', 'config'):
            def corrupt(*args, **kwargs):
                result = self.fake_capture(*args, **kwargs)
                if len(args[0]) > 1 and args[0][1] == 'test':
                    directory = self.evidence[-1]
                    path = directory / 'driver.json'
                    data = json.loads(path.read_text())
                    if mutation == 'empty': (directory / 'report').write_bytes(b'')
                    if mutation == 'incomplete': data['complete'] = 0
                    if mutation == 'live': data['live_allocations'] = 1
                    if mutation == 'bytes': data['tracked_outstanding_bytes'] = 4
                    if mutation == 'domain': data['allocation_domains']['explicit_user_heap']['tracked_outstanding_bytes'] = 4
                    if mutation == 'child': data['child']['exited'] = 0
                    if mutation == 'bitmap': (directory / 'report').write_bytes(raw_report(data).replace(b'dfff2f', b'ffffff'))
                    if mutation == 'main': data['declarations'][0]['selected'] = 1
                    if mutation == 'site': data['sites'][0]['line'] = 10
                    if mutation == 'config': data['min_test_coverage'] = 89
                    if mutation in ('main', 'site'):
                        rehash(data)
                        (directory / 'report').write_bytes(raw_report(data))
                    path.write_text(json.dumps(data))
                    if mutation == 'duplicate-json': path.write_text(path.read_text()[:-1] + ', "complete": 1}')
                return result
            with self.subTest(mutation=mutation):
                code, result, _ = self.invoke(corrupt)
                self.assertEqual(code, 1, result)
                self.assertEqual(len(result['commands']), 17)

    def test_stale_output_symlink_and_replaced_identity(self):
        session = self.session()
        stale = session.artifacts / 'success-ordinary-O0.image'
        stale.write_bytes(b'stale')
        self.assertRaises(AssertionError, session.outputs, 'success-ordinary-O0')
        stale.unlink()
        stale.symlink_to(self.compiler)
        self.assertRaises(AssertionError, session.outputs, 'success-ordinary-O0')
        self.assertRaises(AssertionError, runner.regular, stale)
        self.compiler.write_bytes(b'changed')
        self.assertRaises(AssertionError, session.verify)

    def test_changed_retained_image_fails_even_with_expected_output(self):
        def corrupt(*args, **kwargs):
            result = self.fake_capture(*args, **kwargs)
            if len(args[0]) == 1:
                Path(args[0][0]).write_bytes(b'replaced image')
            return result
        code, result, _ = self.invoke(corrupt)
        self.assertEqual(code, 1)
        self.assertIn('changed/replaced artifact', result['error'])

    def test_duplicate_and_skipped_calls_and_cases_rejected(self):
        session = self.session()
        with mock.patch.object(runner, 'capture', side_effect=self.fake_capture):
            self.assertRaises(AssertionError, session.ordinary, 'error', 0)
            self.assertRaises(AssertionError, session.observed, 0)
            session.ordinary('success', 0)
            self.assertRaises(AssertionError, session.ordinary, 'success', 0)
        self.assertRaises(AssertionError, session.finish)
        for mutation in ('skip', 'duplicate', 'twice'):
            with mock.patch.object(runner.LifecycleRun, 'finish', autospec=True) as finish:
                def modified(run):
                    if mutation == 'skip': run.cases.pop()
                    elif mutation == 'duplicate': run.cases[-1] = run.cases[-2]
                    else: run.cases[-1]['executions'] = 2
                    return real_finish(run)
                finish.side_effect = modified
                code, result, _ = self.invoke()
                self.assertEqual(code, 1, result)

    def test_duplicate_run_id_and_reused_private_evidence_rejected(self):
        for mutation in ('run', 'directory'):
            first = []
            def corrupt(*args, **kwargs):
                result = self.fake_capture(*args, **kwargs)
                if len(args[0]) > 1 and args[0][1] == 'test':
                    directory = self.evidence[-1]
                    if not first: first.append(directory)
                    elif mutation == 'run':
                        path = directory / 'driver.json'
                        data = json.loads(path.read_text()); data['run'] = f'{1:064x}'
                        path.write_text(json.dumps(data)); (directory / 'report').write_bytes(raw_report(data))
                    else:
                        Path(str(args[2]) + '.stderr').write_bytes(b'observation: private evidence ' + os.fsencode(first[0]) + b'\n')
                return result
            with self.subTest(mutation=mutation):
                code, result, _ = self.invoke(corrupt)
                self.assertEqual(code, 1, result)
                self.assertEqual(len(result['commands']), 18)

    def test_failed_rerun_invalidates_stale_current_success(self):
        code, _, first = self.invoke()
        self.assertEqual(code, 0)
        base = self.root / 'artifacts'
        (base / 'RESULT.txt').write_text('PASS stale')
        (base / 'result.json').write_text('{"status":"pass"}')
        with mock.patch.object(runner, 'capture', side_effect=OSError('mock launch failure')), redirect_stdout(io.StringIO()):
            self.assertEqual(runner.run(self.compiler, base), 1)
        current = Path((base / 'LATEST').read_text().strip())
        self.assertNotEqual(current, first)
        self.assertFalse((base / 'RESULT.txt').exists())
        self.assertFalse((base / 'result.json').exists())
        self.assertEqual(json.loads((current / 'result.json').read_text())['status'], 'fail')


real_finish = runner.LifecycleRun.finish

if __name__ == '__main__':
    unittest.main()
