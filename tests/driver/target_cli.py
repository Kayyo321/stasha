#!/usr/bin/env python3
"""Bounded target-argument qualification: scalar obj/IR, editor errors, early exits.

Never links, archives, or runs generated code. Project-loading precedence is
source-only evidence. Every compiler child has controlled stdin. Logger-owned
paths and aliases are outside the output-preservation promise; case logs are
fresh and only the exact normal logger entries are permitted.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BARE = 'x86_64-unknown-none-elf'
LINUX = 'x86_64-unknown-linux-gnu'
BAD = 'mystery-unknown-none'
OPERAND_ERROR = 'invalid --target argument (expected --target <triple> with a nonempty, non-option operand)'
EQUALS_ERROR = 'unsupported --target= spelling (use --target <triple>)'
INVENTORY_SHA256 = '3ac5712ab79c2db87d635c07bb82968296e4fa2636ab055d70cb5a9a1e6b19d1'
SOURCE = 'mod target_cli;\next fn add(stack u64 a, stack u64 b): u64 { ret a + b; }\n'
TS = r'\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}'
INTERNAL_FAILURE = re.compile(r'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|LLVM ERROR:|LLVM IR verification failed|Assertion .*failed|Segmentation fault', re.I)


def cases():
    result = []
    def add(name, args, kind='early', **kwargs):
        result.append(dict(name=name, args=args, kind=kind, **kwargs))
    malformed = [('missing', ['--target'], OPERAND_ERROR),
                 ('empty', ['--target', ''], OPERAND_ERROR),
                 ('next-option', ['--target', '-g'], OPERAND_ERROR),
                 ('equals-empty', ['--target='], EQUALS_ERROR),
                 ('equals-value', ['--target=' + BARE], EQUALS_ERROR),
                 ('equals-control', ['--target=\n\x1b[31m'], EQUALS_ERROR),
                 ('malformed-before-valid', ['--target', '', '--target', BARE], OPERAND_ERROR),
                 ('malformed-after-valid', ['--target', BARE, '--target', ''], OPERAND_ERROR),
                 ('missing-after-valid', ['--target', BARE, '--target'], OPERAND_ERROR)]
    for mode in ('obj', 'ir'):
        for kernel in (False, True):
            prefix = mode + ('-kernel' if kernel else '')
            for index, (name, flags, error) in enumerate(malformed):
                output = ('regular.out', 'symlink.out', 'directory.out')[index % 3]
                # Missing operands must be terminal. Others cover both -o orders.
                args = [mode, 'absent.sts' if index % 2 else 'invalid.sts']
                args += ['--kernel'] if kernel else []
                args += (['-o', output, *flags] if index % 2 == 0 or name.startswith('missing')
                         else [*flags, '-o', output])
                add(prefix + '-' + name, args, message=error)
            add(prefix + '-default-output', [mode, 'absent.sts', *(['--kernel'] if kernel else []), '--target', ''], message=OPERAND_ERROR)
    for mode in ('obj', 'ir'):
        add(mode + '-fresh-output', [mode, 'absent.sts', '-o', 'fresh.out', '--target', ''], message=OPERAND_ERROR)
    add('flags-before-source', ['obj', '--target', '', 'absent.sts', '-o', 'regular.out'], message=OPERAND_ERROR)
    add('open-stdin', ['ir', '--stdin', '--path', 'virtual.sts', '-o', 'regular.out', '--target'], message=OPERAND_ERROR, stdin='open')
    for command in ('check', 'tokens', 'symbols', 'definition'):
        for suffix, flags, message in [('empty', ['--target', ''], OPERAND_ERROR),
                                        ('equals', ['--target=' + BARE], EQUALS_ERROR)]:
            add(command + '-' + suffix, [command, 'absent.sts', *flags], 'editor', message=message)
    add('editor-open-stdin', ['check', '--stdin', '--path', 'virtual.sts', '--target'], 'editor', message=OPERAND_ERROR, stdin='open')
    add('target-before-coordinate', ['definition', 'absent.sts', '--target', '', '--line', 'bad'], 'editor', message=OPERAND_ERROR)
    # Existing common-loop coordinate error wins, rather than a new target scan.
    maximum = (1 << (struct.calcsize('L') * 8)) - 2
    add('coordinate-before-target', ['definition', 'absent.sts', '--line', 'bad', '--target', ''], 'editor',
        message=f'invalid operand for --line (expected decimal digits in range 0..{maximum})')
    add('target-before-kernel-editor', ['check', 'absent.sts', '--kernel', '--target', ''], 'editor', message=OPERAND_ERROR)
    add('help-baseline', ['--help'], 'help')
    add('version-baseline', ['--version'], 'version')
    for name, flags, kind in [('target-help', ['--target', '--help'], 'help'),
                              ('target-version', ['--target', '--version'], 'version'),
                              ('help-first', ['--target=', '--help', '--version'], 'help'),
                              ('version-first', ['--target=', '--version', '--help'], 'version'),
                              ('short-help', ['--target', '', '-h'], 'help')]:
        add(name, ['obj', 'absent.sts', *flags], kind, compare=kind + '-baseline')
    for mode in ('obj', 'ir'):
        add('unknown-' + mode, [mode, '{source}', '-o', 'regular.out', '--target', BAD], 'unknown', output='regular.out')
    def positive(name, mode, flags, triple=BARE, **kwargs):
        output = kwargs.pop('output', 'result.o' if mode == 'obj' else 'result.ll')
        add(name, [mode, '{source}', '-o', output, *flags], mode, output=output, triple=triple, **kwargs)
    positive('explicit-object', 'obj', ['--target', BARE, '-o=2'])
    positive('explicit-ir', 'ir', ['--target', BARE, '-o=2'])
    add('valid-flags-before-source', ['ir', '--target', BARE, '{source}', '-o', 'result.ll', '-o=2'], 'ir', output='result.ll', triple=BARE)
    positive('kernel-default', 'ir', ['--kernel', '-o=2'])
    positive('kernel-explicit', 'ir', ['--kernel', '--target', BARE, '-o=2'], compare='kernel-default')
    positive('kernel-object-default', 'obj', ['--kernel', '-o=2'])
    positive('kernel-object-explicit', 'obj', ['--kernel', '--target', BARE, '-o=2'], compare='kernel-object-default')
    positive('native-default', 'ir', ['-o=2'], triple=None)
    positive('native-explicit', 'ir', ['--target', '{native}', '-o=2'], triple='{native}', compare='native-default')
    positive('repeat-bare-last', 'ir', ['--target', LINUX, '--target', BARE, '-o=2'])
    positive('repeat-linux-last', 'ir', ['--target', BARE, '--target', LINUX, '-o=2'], triple=LINUX)
    positive('unknown-overwritten', 'ir', ['--target', BAD, '--target', BARE, '-o=2'])
    positive('output-operand-not-rescanned', 'ir', ['--target', BARE, '-o=2'], output='--target')
    positive('path-operand-not-rescanned', 'ir', ['--path', '--target', '--target', BARE, '-o=2'])
    positive('similar-option-untouched', 'ir', ['--targeted', '--target', BARE, '-o=2'])
    positive('debug-o0', 'ir', ['--target', BARE, '-g', '-o=0'], debug=True)
    positive('debug-o2', 'ir', ['--target', BARE, '-g', '-o=2'], debug=True, compare='debug-o0')
    return result


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot(root):
    """Do not follow symlinks, including directory symlinks."""
    result = {}
    def visit(directory):
        for path in sorted(directory.iterdir()):
            mode = path.lstat().st_mode
            item = dict(mode=mode)
            if stat.S_ISLNK(mode):
                item['target'] = str(path.readlink())
            elif stat.S_ISREG(mode):
                item.update(size=path.stat().st_size, sha256=sha256(path))
            result[str(path.relative_to(root))] = item
            if stat.S_ISDIR(mode):
                visit(path)
    visit(root)
    return result


def prepare(case):
    case.mkdir()
    (case / 'invalid.sts').write_text('this is not valid Stasha source {\n')
    (case / 'untouched.txt').write_text('preserve every input and sentinel\n')
    for name in ('regular.out', 'target.out', 'a.o', 'a.ll'):
        (case / name).write_text('sentinel ' + name + '\n')
        (case / name).chmod(0o640)
    (case / 'symlink.out').symlink_to('target.out')
    (case / 'directory.out').mkdir(mode=0o750)
    (case / 'directory.out/child').write_text('directory contents\n')


def category(code, timed_out=False):
    if timed_out:
        return 'timeout'
    if code is None:
        return 'launch-error'
    if code < 0:
        return 'signal'
    return {0: 'success', 1: 'rejection'}.get(code, 'unexpected-exit')


def capture(command, cwd, prefix, timeout, stdin_mode='closed'):
    """Raw streams on disk, no inherited input and no output sanitization."""
    if stdin_mode not in ('closed', 'open'):
        raise ValueError('unreviewed stdin policy')
    record = dict(command=command, cwd=str(cwd), timeout_seconds=timeout, stdin=stdin_mode,
                  environment_overrides=dict(NO_COLOR='1', TERM='dumb'))
    start = time.monotonic()
    stdout_path, stderr_path = Path(str(prefix) + '.stdout'), Path(str(prefix) + '.stderr')
    with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
        process = None
        try:
            process = subprocess.Popen(command, cwd=cwd, env=dict(os.environ, NO_COLOR='1', TERM='dumb'),
                                       stdin=subprocess.PIPE if stdin_mode == 'open' else subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, start_new_session=True)
            timed_out = False
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                code = process.wait()
            record.update(returncode=code, category=category(code, timed_out), reaped=True)
        except OSError as error:
            record.update(returncode=None, category='launch-error' if process is None else 'process-error', error=str(error), reaped=process is None)
        finally:
            if process is not None:
                # An OS error after spawning must not strand a live child.
                try:
                    if process.poll() is None:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except OSError:
                            process.kill()
                        process.wait(timeout=5)
                    record['reaped'] = True
                except (OSError, subprocess.TimeoutExpired) as error:
                    record.update(category='cleanup-error', cleanup_error=str(error), reaped=False)
                if process.stdin is not None:
                    process.stdin.close()
    record['elapsed_seconds'] = round(time.monotonic() - start, 6)
    for stream, path in [('stdout', stdout_path), ('stderr', stderr_path)]:
        record[stream + '_file'] = str(path)
        raw = path.read_bytes()
        record[stream] = raw.decode('utf-8', errors='replace')
    Path(str(prefix) + '.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def diagnostic(message):
    return dict(diagnostics=[dict(severity='error', message=message, file='<unknown>',
                                 line=0, col=0, len=1, labels=[], notes=[])])


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError('non-JSON constant: ' + value)


def validate_logs(case, after, kind, issues):
    allowed = kind in ('early', 'unknown', 'obj', 'ir')
    names = {name for name in after if name == 'logs' or name.startswith('logs/')}
    if not names:
        return
    expected = {'logs', 'logs/.rotation-lock', 'logs/log0-last.log'}
    if not allowed or names != expected:
        issues.append('unexpected logger entries: ' + repr(sorted(names)))
        return
    if not stat.S_ISDIR(after['logs']['mode']) or any(not stat.S_ISREG(after[name]['mode']) for name in expected - {'logs'}):
        issues.append('logger entry has wrong type')
        return
    if after['logs/.rotation-lock']['size'] != 0:
        issues.append('nonempty logger lock')
    lines = (case / 'logs/log0-last.log').read_text().splitlines()
    resolver_line = f"(EE) unsupported target '{BAD}': no qualified target descriptor"
    if (len(lines) < 2 or not re.fullmatch('=== Log opened at ' + TS + ' ===', lines[0])
            or not re.fullmatch('=== Log closed at ' + TS + ' ===', lines[-1])
            or any(not re.fullmatch(r'(?:\(EE\) |\(!!\) )?\[' + TS + r'\] [^\x00-\x08\x0b-\x1f]*', line) for line in lines[1:-1] if not (kind == 'unknown' and line == resolver_line))):
        issues.append('invalid normal logger contents')


def validate_output(path, spec, native):
    if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
        raise ValueError('missing, empty, or non-regular positive output')
    data = path.read_bytes()
    if spec['kind'] == 'obj':
        if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError('expected ELF64 little-endian version-1 object')
        if struct.unpack_from('<HHI', data, 16) != (1, 62, 1):
            raise ValueError('expected relocatable x86-64 ELF object')
        if struct.unpack_from('<H', data, 52)[0] != 64:
            raise ValueError('invalid ELF header size')
        offset = struct.unpack_from('<Q', data, 40)[0]
        entry_size, count, strings_index = struct.unpack_from('<HHH', data, 58)
        if entry_size != 64 or count < 4 or not 0 < strings_index < count or offset < 64 or offset + count * 64 > len(data):
            raise ValueError('invalid ELF section table')
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * 64) for i in range(count)]
        strings = sections[strings_index]
        if strings[1] != 3 or strings[4] + strings[5] > len(data):
            raise ValueError('invalid ELF section name table')
        names = data[strings[4]:strings[4] + strings[5]]
        found = set()
        for section in sections[1:]:
            name, kind, _, _, start, size, _, _, _, _ = section
            if name >= len(names) or b'\0' not in names[name:]:
                raise ValueError('invalid ELF section name')
            if kind != 8 and start + size > len(data):
                raise ValueError('ELF section extends past file')
            label = names[name:].split(b'\0', 1)[0]
            if (label, kind) in ((b'.text', 1), (b'.symtab', 2)) and size:
                found.add(label)
        if found != {b'.text', b'.symtab'}:
            raise ValueError('missing ELF code or symbol table')
        return dict(sha256=sha256(path), size=len(data), elf_class=64, endian='little', machine=62, type=1)
    text = data.decode('utf-8')
    triples = re.findall(r'^target triple = "([^"\n]+)"$', text, re.M)
    if len(triples) != 1:
        raise ValueError('expected exactly one IR target triple')
    expected = native if spec['triple'] == '{native}' else spec['triple']
    if expected is not None and triples[0] != expected:
        raise ValueError('wrong IR triple: ' + triples[0])
    if not re.search(r'^define\b[^\n]*\bi64 @add\(i64\b[^\n]*, i64\b', text, re.M):
        raise ValueError('missing ordinary scalar add definition')
    if spec.get('debug') and ('!DICompileUnit(' not in text or '!dbg' not in text):
        raise ValueError('missing requested debug metadata')
    return dict(sha256=sha256(path), size=len(data), triple=triples[0])


def check_streams(process, spec, previous, issues):
    expected_exit = 1 if spec['kind'] in ('early', 'editor', 'unknown') else 0
    if type(process.get('returncode')) is not int or process['returncode'] != expected_exit or process.get('category') != category(expected_exit):
        issues.append(f"exit={process.get('returncode')} category={process.get('category')} expected={expected_exit}")
    for stream in ('stdout', 'stderr'):
        path = process.get(stream + '_file')
        if not path or Path(path).read_bytes() != process[stream].encode('utf-8'):
            issues.append('missing or changed raw ' + stream)
    stdout, stderr = process['stdout'], process['stderr']
    if re.search(r'[\x00-\x08\x0b-\x1f\x7f]', stdout + stderr):
        issues.append('raw stream control-byte contamination')
    if INTERNAL_FAILURE.search(stdout + '\n' + stderr):
        issues.append('internal compiler or sanitizer failure')
    kind = spec['kind']
    if kind == 'editor':
        if stderr:
            issues.append('editor stderr contamination')
        try:
            actual = json.loads(stdout, object_pairs_hook=unique_object, parse_constant=reject_constant)
            if json.dumps(actual, sort_keys=True) != json.dumps(diagnostic(spec['message']), sort_keys=True):
                issues.append('editor JSON differs from exact diagnostic oracle')
        except ValueError as error:
            issues.append('invalid single JSON document: ' + str(error))
    elif kind == 'early':
        if stdout:
            issues.append('ordinary rejection wrote stdout')
        # Preserve logger summary behavior without fixing its historical count.
        pattern = r'\(EE\) \[' + TS + r'\] ' + re.escape(spec['message']) + r'\n(?:error: aborting due to [1-9][0-9]* previous errors?\n)?'
        if not re.fullmatch(pattern, stderr):
            issues.append('ordinary diagnostic differs from exact target error contract')
    elif kind == 'unknown':
        if stdout or not re.search(r"unsupported target 'mystery-unknown-none': no qualified target descriptor", stderr):
            issues.append('missing existing unsupported-target diagnostic')
    elif kind in ('help', 'version'):
        if stderr:
            issues.append('early exit wrote stderr')
        if kind == 'version' and not re.fullmatch(r'stasha [^\n]+ \(ABI [0-9]+\)\n', stdout):
            issues.append('invalid version response')
        if kind == 'help' and not (stdout.startswith('stasha ') and '\nUsage:\n' in stdout and '--target <triple>' in stdout):
            issues.append('invalid help response')
        if spec.get('compare') and stdout != previous[spec['compare']]['process']['stdout']:
            issues.append('early help/version precedence changed')
    else:
        if stdout:
            issues.append('positive compile wrote stdout')
        normal = r'\[' + TS + r'\] [^\n]*\n'
        if stderr and not re.fullmatch('(?:' + normal + ')+', stderr):
            issues.append('unexpected positive diagnostic')


def run_case(compiler, work, source, spec, timeout, runner, previous, identity, native):
    case, evidence = work / spec['name'], work / (spec['name'] + '-evidence')
    prepare(case)
    evidence.mkdir()
    before = snapshot(case)
    command = [compiler, *[str(source) if value == '{source}' else native if value == '{native}' else value for value in spec['args']]]
    if any(value is None for value in command):
        raise ValueError('native default triple was not established')
    record = dict(name=spec['name'], kind=spec['kind'], issues=[], before=before, command=command,
                  compiler_sha256_before=sha256(compiler))
    issues = record['issues']
    if record['compiler_sha256_before'] != identity:
        raise ValueError('compiler changed before invocation')
    process = runner(command, case, evidence / 'compile', timeout, spec.get('stdin', 'closed'))
    record['process'] = process
    if (process.get('command') != command or process.get('cwd') != str(case)
            or process.get('stdin') != spec.get('stdin', 'closed')
            or process.get('timeout_seconds') != timeout or not process.get('reaped')):
        issues.append('process receipt differs from exact invocation policy')
    record['compiler_sha256_after'] = sha256(compiler)
    if record['compiler_sha256_after'] != identity:
        issues.append('compiler changed during invocation')
    check_streams(process, spec, previous, issues)
    after = snapshot(case)
    record['after'] = after
    ordinary = spec['kind'] in ('early', 'unknown', 'obj', 'ir')
    validate_logs(case, after, spec['kind'], issues)
    changed = {spec['output']} if spec['kind'] == 'unknown' else set()
    created = {spec['output']} if spec['kind'] in ('obj', 'ir') else set()
    for name, original in before.items():
        if name in changed:
            if name in after:
                issues.append('unknown-target compile failed to invalidate old output')
        elif after.get(name) != original:
            issues.append('preserved entry changed or disappeared: ' + name)
    for name in sorted(set(after) - set(before) - created):
        if not (ordinary and name in ('logs', 'logs/.rotation-lock', 'logs/log0-last.log')):
            issues.append('unexpected generated entry: ' + name)
    if spec['kind'] in ('obj', 'ir'):
        output = case / spec['output']
        if spec['output'] in before:
            issues.append('positive output was not fresh')
        try:
            record['output'] = validate_output(output, spec, native)
            if spec.get('compare') and record['output']['sha256'] != previous[spec['compare']].get('output', {}).get('sha256'):
                issues.append('default/explicit or debug-equivalent outputs differ')
        except (OSError, ValueError, UnicodeError) as error:
            issues.append(str(error))
    return record


def run_suite(compiler, artifacts=None, timeout=15, runner=None):
    if timeout <= 0 or not math.isfinite(timeout):
        raise ValueError('timeout must be positive and finite')
    compiler = str(Path(compiler).resolve())
    if artifacts:
        Path(artifacts).mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='stasha-target-cli-', dir=artifacts))
    source_dir = work / 'input'
    source_dir.mkdir()
    source = source_dir / 'scalar.sts'
    source.write_text(SOURCE)
    source_before = snapshot(source_dir)
    identity = sha256(compiler)
    inventory = cases()
    if hashlib.sha256(json.dumps(inventory, sort_keys=True).encode()).hexdigest() != INVENTORY_SHA256:
        raise ValueError('approved command inventory changed')
    (work / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    results, previous, native = [], {}, None
    for spec in inventory:
        try:
            record = run_case(compiler, work, source, spec, timeout, runner or capture, previous, identity, native)
        except (OSError, ValueError, KeyError) as error:
            record = dict(name=spec['name'], issues=['runner failure: ' + str(error)])
            results.append(record)
            break
        results.append(record)
        previous[spec['name']] = record
        if spec['name'] == 'native-default' and not record['issues']:
            native = record.get('output', {}).get('triple')
        if record['compiler_sha256_after'] != identity:
            break
    final_issues = []
    try:
        after = sha256(compiler)
        if after != identity:
            final_issues.append('compiler changed during suite')
        if snapshot(source_dir) != source_before:
            final_issues.append('shared scalar source changed')
    except OSError as error:
        after = None
        final_issues.append('identity read failed: ' + str(error))
    if [record['name'] for record in results] != [spec['name'] for spec in inventory]:
        final_issues.append('not every approved invocation was visited')
    failed = sum(bool(record['issues']) for record in results) + bool(final_issues)
    report = dict(suite='bounded-target-cli', evidence='injected-process controls only' if runner else 'real compiler invocations',
                  compiler=compiler, compiler_sha256_before=identity, compiler_sha256_after=after,
                  inventory_count=len(inventory), visited=len(results), passed=sum(not record['issues'] for record in results),
                  failed=failed, status='FAIL' if failed else 'PASS', issues=final_issues, results=results,
                  project_loading_precedence='source-only', full_k0_readiness=False,
                  excluded_stages={name: 'NOT_RUN' for name in ('link', 'archive', 'generated-program-execution', 'boot', 'hardware', 'debugger')})
    # No successful return if the terminal receipt cannot be retained.
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, work


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=15)
    args = parser.parse_args(argv)
    try:
        report, work = run_suite(args.compiler, args.artifacts, args.timeout)
    except (OSError, ValueError) as error:
        print('FAIL target CLI runner: ' + str(error), file=sys.stderr)
        return 1
    for result in report['results']:
        print(('FAIL' if result['issues'] else 'PASS') + ' ' + result['name'])
        for issue in result['issues']:
            print('  ' + issue)
    for issue in report['issues']:
        print('FAIL ' + issue)
    print(f"{report['passed']} passed, {report['failed']} failed; artifacts: {work}")
    return int(report['status'] != 'PASS')


if __name__ == '__main__':
    sys.exit(main())
