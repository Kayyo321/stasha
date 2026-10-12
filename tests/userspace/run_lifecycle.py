#!/usr/bin/env python3
"""Finite hosted lifecycle checks. Source/IR review precedes first native use.

Eight ordinary main executions must pass before four once-only observed drivers.
This is a test fixture, not daemon, OS-resource or whole-process leak certification.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import stat
import sys
import tempfile

if not __debug__:
    raise RuntimeError('qualification requires Python assertions enabled')
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'tests/userspace/fixtures'
LEVELS = [0, 1, 2, 3]
ORACLE_SHA256 = '7ea4b0f02f66750b15db0b5d7d71ed98943b9fe544b8b78526715b15d4050401'
FIXTURE_HASHES = {
    'lifecycle.sts': '2ac4b04d02134d9b1b9dab5c0d14c6bec33f85081632893eb56cd48e738ae521',
    'success.sts': '91d1075ec0154ec639c623cc8df29e776e75919ee32ad5ab37e0f727c5fd646b',
    'error.sts': 'df7577a19299535ba7739601e1c1d85e3e0d59efd3072f566233fe21c778ebb3',
}
ORDINARY = {
    'success': (0, 'success', b'MAIN start\nSTART\nREQUEST 1\nREQUEST 2\nREQUEST 3\nTOTAL 6\nCLEANUP 3\nMAIN status 0 cleanups 1\n'),
    'error': (23, 'unexpected-exit', b'MAIN start\nSTART\nREQUEST 1\nREJECT 2\nCLEANUP 1\nMAIN status 23 cleanups 1\n'),
}
OBSERVED_STDOUT = (b"test 'success lifecycle' ... OBS_SUCCESS\nSTART\nREQUEST 1\nREQUEST 2\nREQUEST 3\nTOTAL 6\nCLEANUP 3\nPASS\n"
                   b"test 'error lifecycle' ... OBS_ERROR\nSTART\nREQUEST 1\nREJECT 2\nCLEANUP 1\nPASS\n\n4 passed, 0 failed\n")
LEAF_LINES = [4, 7, 8, 9, 10, 11, 13, 14, 16, 17, 18, 20, 21, 22, 23, 25, 26, 27, 29, 30, 31, 33]
LEAF_HITS = [4, 7, 8, 9, 10, 13, 14, 16, 17, 18, 20, 21, 22, 23, 25, 26, 27, 29, 30, 33]
FLAGS = ['--max-leaked-bytes', '0', '--min-test-coverage', '90']
ZERO_DOMAINS = {name: {'live_allocations': 0, 'tracked_outstanding_bytes': 0}
                for name in ('explicit_user_heap', 'generated_primitive_slice_backing')}
INPUT_PATHS = ['tests/userspace/run_lifecycle.py', 'tests/userspace/test_lifecycle_runner.py',
               'tests/userspace/README.md', 'scripts/qualify_linux.sh',
               'tests/harness/test_qualification_script.py', 'tests/harness/compiler_suites.py',
               'tests/observation/test_source_imports.py', 'Makefile',
               *['tests/userspace/fixtures/' + name for name in FIXTURE_HASHES]]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


support = load_module('lifecycle_observation_support', ROOT / 'tests/observation/test_source_imports.py')
harness = load_module('lifecycle_capture_support', ROOT / 'tests/harness/compiler_suites.py')
capture = harness.capture


def regular(path, *, executable=False):
    """Reject symlink substitution, including parent-component rebinding."""
    path = Path(path)
    assert path.is_absolute() and path.resolve(strict=True) == path, ('noncanonical artifact', path)
    info = path.lstat()
    assert stat.S_ISREG(info.st_mode) and info.st_size > 0, ('missing/empty/nonregular artifact', path)
    if executable:
        assert os.access(path, os.X_OK), ('nonexecutable image', path)
    return info


def fingerprint(path, *, empty=False):
    path = Path(path)
    if empty:
        assert path.resolve(strict=True) == path and stat.S_ISREG(path.lstat().st_mode), path
        info = path.lstat()
    else:
        info = regular(path)
    return {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'device': info.st_dev,
            'inode': info.st_ino, 'size': info.st_size, 'mode': info.st_mode,
            'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns}


def input_paths():
    # Working-tree identities are review inputs, not a claimed compiler build provenance.
    return sorted({ROOT / name for name in INPUT_PATHS} | set(ROOT.glob('src/**/*.c')) |
                  set(ROOT.glob('src/**/*.h')) | set(ROOT.glob('std/crypto/sha256_rt.[ch]')))


def expected_cases():
    return [f'{name}-ordinary-O{opt}' for opt in LEVELS for name in ORDINARY] + [f'helper-observed-O{opt}' for opt in LEVELS]


def expected_commands():
    return [step for name in expected_cases()[:8] for step in (name + '-build', name + '-run')] + expected_cases()[8:]


def validate_status(result, code, category):
    assert (result['returncode'], result['category']) == (code, category), ('wrong exit/category', result)


def validate_compiler_streams(stdout, stderr, *, observed):
    # Ordinary builds must not execute main and then let the runner execute it again.
    assert stdout == (OBSERVED_STDOUT if observed else b''), 'unexpected compiler stdout / premature execution'
    diagnostic = harness.ANSI.sub('', stderr.decode(errors='replace'))
    assert not harness.INTERNAL_FAILURE.search(diagnostic), 'compiler internal/verifier/sanitizer failure'
    assert not re.search(r'\b(?:error|warning)(?:\[[^\]]+\])?:|\(EE\)|\(!!\)|\b[0-9]+ warnings? emitted\b',
                         diagnostic, re.I), 'unexpected compiler error/warning diagnostic'


def validate_observed(data, report, session, binary, opt):
    raw = support.validate_driver(data, report.decode('ascii'), session, binary, FIXTURES / 'success.sts',
        expected=0, pairs=[(1, line) for line in LEAF_LINES], hits=[(1, line) for line in LEAF_HITS],
        assertions=4, tests=2, live=0, byte_count=0)
    assert raw.hex() == 'dfff2f' and data['allocation_domains'] == ZERO_DOMAINS
    assert data['enabled'] == 3 and data['warn_memleak'] is False
    assert data['max_leaked_bytes'] == 0 and data['min_test_coverage'] == 90
    assert data['config_sha256'] == support.config_digest(session.compiler_hash, session.runtime_hash, opt,
        enabled=3, warn=0, maximum=0, minimum=90, target=data['target'])[0]
    assert len(data['files']) == 2
    for file, name in zip(data['files'], ('success.sts', 'lifecycle.sts')):
        assert file['canonical_path_bytes_hex'] == os.fsencode(FIXTURES / name).hex()
        assert file['source_sha256'] == FIXTURE_HASHES[name]
    assert [(site['file_id'], site['line'], site['kind'], site['static_test_id']) for site in data['sites']] == [
        (1, 9, 1, None), (1, 16, 3, None)]
    assert [(t['id'], t['file_id'], t['line'], t['name_bytes_hex']) for t in data['tests']] == [
        (0, 0, 9, b'success lifecycle'.hex()), (1, 0, 14, b'error lifecycle'.hex())]
    root = data['files'][0]
    assert root['lines'] == [] and root['hit_lines'] == []
    # The observer replaces main. Only the ordinary images establish its lifecycle.
    main = [d for d in data['declarations'] if d['file_id'] == 0 and d['name_bytes_hex'] == b'main'.hex()]
    assert len(main) == 1 and main[0]['selected'] == 0
    assert all(root['exclusions'][line - 1] & 8 for line in range(3, 9))
    assert all(root['exclusions'][line - 1] & 4 for line in range(9, 19))
    root_anchors = [a for a in data['anchors'] if a['file_id'] == 0]
    assert root_anchors and all(a['exclusion_reason'] == 2 and a['coverage_id'] is None for a in root_anchors)


class LifecycleRun:
    """A small fail-closed receipt with exact execution order and file identities."""
    def __init__(self, compiler, artifacts):
        artifacts = Path(artifacts).absolute()
        artifacts.mkdir(parents=True, exist_ok=True)
        assert artifacts.resolve() == artifacts and not artifacts.is_symlink()
        # A failed rerun must not leave a current successful receipt.
        for name in ('RESULT.txt', 'result.json'):
            (artifacts / name).unlink(missing_ok=True)
        self.base = artifacts
        self.artifacts = Path(tempfile.mkdtemp(prefix='run-', dir=artifacts))
        (artifacts / 'LATEST').write_text(str(self.artifacts) + '\n')
        self.records, self.cases, self.saved = [], [], {}
        self.seen_runs, self.seen_evidence = set(), set()
        self.evidence_directories = {}
        self.compiler = Path(compiler).absolute()
        regular(self.compiler, executable=True)
        self.runtime = self.compiler.parent / 'observation_runtime.a'
        regular(self.runtime)
        self.compiler_hash = support.sha(self.compiler)
        self.runtime_hash = support.sha(self.runtime)
        self.sources = input_paths()
        for path in [*self.sources, self.compiler, self.runtime]:
            self.retain(path)
        for name, digest in FIXTURE_HASHES.items():
            assert support.sha(FIXTURES / name) == digest, ('frozen fixture changed', name)
        self.write_json('identities.json', {'compiler': str(self.compiler), 'compiler_sha256': self.compiler_hash,
            'runtime': str(self.runtime), 'runtime_sha256': self.runtime_hash, 'oracle_sha256': ORACLE_SHA256,
            'sources': {str(p.relative_to(ROOT)): support.sha(p) for p in self.sources}})

    def retain(self, path):
        path = Path(path)
        assert str(path) not in self.saved, ('duplicate artifact', path)
        self.saved[str(path)] = fingerprint(path, empty=True)

    def write_json(self, name, data):
        path = self.artifacts / name
        assert not path.exists() and not path.is_symlink(), ('stale artifact', path)
        path.write_text(json.dumps(data, indent=2) + '\n')
        self.retain(path)

    def verify(self):
        assert input_paths() == self.sources, 'source inventory changed'
        for name, identity in self.saved.items():
            assert fingerprint(Path(name), empty=True) == identity, ('changed/replaced artifact', name)
        for name, identity in self.evidence_directories.items():
            path = Path(name)
            assert path.resolve(strict=True) == path
            info = path.lstat()
            assert (info.st_dev, info.st_ino, info.st_mode, info.st_uid) == identity, 'changed evidence directory'

    def command(self, name, argv, *, timeout=120, env=None):
        index = len(self.records)
        assert index < len(expected_commands()) and name == expected_commands()[index], 'duplicate/skipped/reordered execution'
        self.verify()
        prefix = self.artifacts / name
        for suffix in ('.stdout', '.stderr', '.json'):
            assert not Path(str(prefix) + suffix).exists() and not Path(str(prefix) + suffix).is_symlink()
        result = capture(argv, ROOT, prefix, timeout, extra_env=env)
        self.records.append({'name': name, 'result': result})
        for suffix in ('.stdout', '.stderr', '.json'):
            self.retain(Path(str(prefix) + suffix))
        assert support.strict_json(Path(str(prefix) + '.json').read_bytes()) == result
        assert result['command'] == list(map(str, argv)) and result['cwd'] == str(ROOT)
        assert result['timeout_seconds'] == timeout
        self.verify()
        return result, Path(str(prefix) + '.stdout').read_bytes(), Path(str(prefix) + '.stderr').read_bytes()

    def outputs(self, name):
        paths = [self.artifacts / (name + suffix) for suffix in ('.image', '.pre.ll', '.post.ll')]
        assert all(not path.exists() and not path.is_symlink() for path in paths), 'stale build output'
        return paths

    def retain_outputs(self, paths, *, observed):
        for index, path in enumerate(paths):
            regular(path, executable=index == 0)
            if index:
                ir = path.read_bytes()
                assert (b'__stasha_obs_init_v4' in ir) == observed, ('wrong IR mode', path)
                if not observed:
                    assert b'__stasha_obs_' not in ir
            self.retain(path)

    def ordinary(self, name, opt):
        key = f'{name}-ordinary-O{opt}'
        binary, pre, post = self.outputs(key)
        result, stdout, stderr = self.command(key + '-build',
            [self.compiler, 'build', FIXTURES / (name + '.sts'), f'-o={opt}', '-o', binary],
            env={'STS_DUMP_IR_PRE': str(pre), 'STS_DUMP_IR': str(post)})
        validate_status(result, 0, 'success')
        validate_compiler_streams(stdout, stderr, observed=False)
        self.retain_outputs([binary, pre, post], observed=False)
        result, stdout, stderr = self.command(key + '-run', [binary], timeout=10)
        code, category, expected_stdout = ORDINARY[name]
        validate_status(result, code, category)
        assert stdout == expected_stdout and stderr == b'', ('wrong ordinary trace', key)
        self.cases.append({'name': key, 'executions': 1, 'status': 'pass'})

    def observed(self, opt):
        assert [case['name'] for case in self.cases[:8]] == expected_cases()[:8], 'ordinary stage incomplete'
        key = f'helper-observed-O{opt}'
        binary, pre, post = self.outputs(key)
        before = {str(p) for p in Path('/tmp').glob('stasha-observation-*')}
        result, stdout, stderr = self.command(key,
            [self.compiler, 'test', FIXTURES / 'success.sts', f'-o={opt}', '-o', binary, *FLAGS],
            env={'STS_DUMP_IR_PRE': str(pre), 'STS_DUMP_IR': str(post)})
        validate_status(result, 0, 'success')
        validate_compiler_streams(stdout, stderr, observed=True)
        self.retain_outputs([binary, pre, post], observed=True)
        matches = re.findall(rb'^observation: private evidence ([^\r\n]+)$', stderr, re.M)
        assert len(matches) == 1, 'missing/duplicate driver evidence'
        evidence = Path(os.fsdecode(matches[0]))
        assert evidence.parent == Path('/tmp') and re.fullmatch(r'stasha-observation-[A-Za-z0-9]+', evidence.name)
        assert str(evidence) not in before | self.seen_evidence, 'stale/reused evidence directory'
        assert evidence.resolve(strict=True) == evidence
        info = evidence.lstat()
        assert stat.S_ISDIR(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o700 and info.st_uid == os.getuid()
        self.seen_evidence.add(str(evidence))
        self.evidence_directories[str(evidence)] = (info.st_dev, info.st_ino, info.st_mode, info.st_uid)
        retained = self.artifacts / (key + '-evidence')
        retained.mkdir(mode=0o700)
        for filename, limit in (('driver.json', 32 * 1024 * 1024), ('report', 2 * 1024 * 1024)):
            source = evidence / filename
            assert regular(source).st_size <= limit
            self.retain(source)
            (retained / filename).write_bytes(source.read_bytes())
            self.retain(retained / filename)
        data = support.strict_json((retained / 'driver.json').read_bytes())
        assert data['run'] not in self.seen_runs, 'stale/reused run ID'
        validate_observed(data, (retained / 'report').read_bytes(), self, binary, opt)
        self.seen_runs.add(data['run'])
        self.cases.append({'name': key, 'executions': 1, 'status': 'pass', 'run_id': data['run'],
            'evidence': str(evidence), 'binary_sha256': data['binary_sha256'],
            'input_graph_sha256': data['input_graph_sha256'], 'map_sha256': data['map_sha256'],
            'config_sha256': data['config_sha256'], 'eligible': 22, 'covered': 20,
            'assertions_passed': 4, 'tests': 2, 'live_allocations': 0, 'tracked_outstanding_bytes': 0})

    def finish(self):
        self.verify()
        assert [c['name'] for c in self.cases] == expected_cases(), 'incomplete/duplicate case inventory'
        assert all(c['executions'] == 1 and c['status'] == 'pass' for c in self.cases)
        assert [r['name'] for r in self.records] == expected_commands(), 'incomplete command inventory'
        assert len(self.seen_runs) == len(self.seen_evidence) == 4
        self.write_json('artifact-identities.json', self.saved)
        return {'status': 'pass', 'ordinary_executions': 8, 'observed_driver_invocations': 4,
                'cases': self.cases, 'commands': self.records}


def run(compiler, artifacts):
    session = None
    try:
        session = LifecycleRun(compiler, artifacts)
        for opt in LEVELS:
            for name in ORDINARY:
                session.ordinary(name, opt)
        for opt in LEVELS:
            session.observed(opt)
        result = session.finish()
    except Exception as error:
        result = {'status': 'fail', 'error': repr(error), 'cases': session.cases if session else [],
                  'commands': session.records if session else []}
    destination = session.artifacts if session else Path(artifacts).absolute()
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(result['status'].upper(), destination / 'result.json')
    return 0 if result['status'] == 'pass' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--opt', default='0,1,2,3', choices=['0,1,2,3'])
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'build/qualification/userspace-lifecycle')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('requires Linux x86-64')
    return run(args.compiler, args.artifacts)


if __name__ == '__main__':
    raise SystemExit(main())
