#!/usr/bin/env python3
"""End-to-end output, target, static-initializer and kernel boundary tests."""
import argparse
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture

p = argparse.ArgumentParser()
p.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
p.add_argument('--artifacts', type=Path)
p.add_argument('--timeout', type=float, default=60)
a = p.parse_args()
compiler = str(Path(a.compiler).resolve())
if a.artifacts:
    a.artifacts.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='stasha-driver-', dir=a.artifacts))
results = []

def run(name, mode, source, flags=(), expect=0, pattern=None, output=None):
    case = work / name
    case.mkdir()
    path = case / 'input.sts'
    path.write_text(source)
    output = output or case / ('result.ll' if mode == 'ir' else 'result.o' if mode == 'obj' else 'result')
    if expect:
        output.write_text('stale artifact must never survive a failed build')
    command = [compiler, mode, str(path), '-o', str(output), *flags]
    result = capture(command, ROOT, case / 'compile', a.timeout)
    issues = []
    if result['returncode'] != expect or result['category'] in ('timeout', 'signal', 'launch-error'):
        issues.append(f"exit={result['returncode']} category={result['category']} expected={expect}")
    if expect == 0 and not output.is_file(): issues.append('missing output')
    if expect != 0 and output.exists(): issues.append('stale/failed output survived')
    if pattern and not re.search(pattern, result['stderr'], re.I): issues.append('missing diagnostic: '+pattern)
    if re.search(r'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|LLVM IR verification failed', result['stderr']):
        issues.append('sanitizer or verifier failure')
    results.append({'name':name, 'issues':issues, 'process':result})
    return output, result, issues

def check(name, test):
    try:
        test()
        results.append({'name':name, 'issues':[]})
    except (AssertionError, OSError, ValueError) as error:
        results.append({'name':name, 'issues':[str(error) or 'assertion failed']})

plain = 'mod t; fn main(void): i32 { ret 0; }'
for mode in ('build', 'obj', 'ir'):
    run('basic-'+mode, mode, plain)
    run('syntax-stale-'+mode, mode, plain+' /* unterminated', expect=1, pattern='unterminated')
    run('unknown-target-'+mode, mode, plain, ['--target','mystery-unknown-none'], expect=1, pattern='unsupported target')

source = 'mod t; ext fn add(stack u64 a, stack u64 b): u64 { ret a + b; }'
obj, _, _ = run('kernel-object', 'obj', source, ['--kernel','-o=2'])
check('kernel-ELF-header', lambda: (obj.read_bytes()[:4] == b'\x7fELF') or (_ for _ in ()).throw(AssertionError('not ELF')))
if obj.is_file():
    inspected = capture(['objdump','-d',str(obj)], ROOT, work/'kernel-disassembly', a.timeout)
    results.append({'name':'kernel-register-policy', 'issues': [] if inspected['returncode']==0 and not re.search(r'\b[xyz]mm\d+\b|\bmm[0-7]\b', inspected['stdout']) else ['disassembly failed or used FP/vector registers'], 'process':inspected})
    imports = capture(['nm','-u',str(obj)], ROOT, work/'kernel-imports', a.timeout)
    unexpected = [line for line in imports['stdout'].splitlines() if line.strip() and '__stasha_panic' not in line]
    results.append({'name':'kernel-import-policy', 'issues': [] if imports['returncode']==0 and not unexpected else ['unexpected imports or nm failure',*unexpected], 'process':imports})

ir, _, _ = run('kernel-ir', 'ir', source, ['--kernel','-o=0'])
check('kernel-IR-policy', lambda: all(term in ir.read_text() for term in ('noredzone','noimplicitfloat','nounwind','stasha.abi')) or (_ for _ in ()).throw(AssertionError('missing machine/ABI attribute')))
run('kernel-no-hosted-build', 'build', source, ['--kernel'], expect=1, pattern='supports obj, ir, and lib only')
run('kernel-no-hosted-dylib', 'dylib', source, ['--kernel'], expect=1, pattern='supports obj, ir, and lib only')
run('kernel-no-hosted-test', 'test', source, ['--kernel'], expect=1, pattern='supports obj, ir, and lib only')
run('kernel-heap-rejected', 'obj', 'mod t; fn f(void): void { heap u64 n = 1; }', ['--kernel'], expect=1, pattern='heap')
run('kernel-CPU-override', 'obj', 'mod t; @[[target: "haswell"]] fn f(void): void {}', ['--kernel'], expect=1, pattern='machine')
run('kernel-hook-ABI', 'obj', 'mod t; ext fn __stasha_panic(stack u64 reason): void;', ['--kernel'], expect=1, pattern='exact external ABI')
run('org-rejected', 'obj', '@[[freestanding]]; @[[org: 0x7c00]]; fn f(void): void {}', expect=1, pattern='org is not implemented')

fs, _, _ = run('freestanding-no-crash-constructor','ir','@[[freestanding]]; fn f(void): void {}',['-o=0'])
check('freestanding-crash-absence', lambda: ('__crash' not in fs.read_text() and 'global_ctors' not in fs.read_text()) or (_ for _ in ()).throw(AssertionError('implicit crash constructor')))

# Compare native/default and explicit normalized native target on the exact same source path.
native_dir = work/'native-equivalence'; native_dir.mkdir()
native_source = native_dir/'input.sts'; native_source.write_text(source)
def native_equivalence():
    paths=[]
    triple=None
    for index in range(2):
        output=native_dir/f'{index}.ll'; paths.append(output)
        command=[compiler,'ir',str(native_source),'-o',str(output),'-o=2']
        if index: command += ['--target',triple]
        result=capture(command,ROOT,native_dir/f'compile-{index}',a.timeout)
        assert result['returncode']==0 and output.is_file(), result['stderr']
        if not index: triple=re.search(r'target triple = "([^"]+)"',output.read_text()).group(1)
    assert paths[0].read_bytes()==paths[1].read_bytes(), 'native and explicit-native IR differ'
check('native-equivalence',native_equivalence)

static_source='''mod globals;
stack u64 *rw later_address = &later;
stack i8 *r label = "kernel";
stack u64 words[3] = .{1, 4294967296, 18446744073709551615};
stack u64 later = 18446744073709551615;
'''
globals_ir,_,_=run('static-addresses-and-data','ir',static_source,['--kernel','-o=0'])
check('static-data-retained', lambda: all(x in globals_ir.read_text() for x in ('@later','4294967296','kernel','i64 -1')) or (_ for _ in ()).throw(AssertionError('missing static data/relocation')))
run('static-call-rejected','obj','mod t; stack u64 x = f(); fn f(void): u64 { ret 7; }',expect=1,pattern='constant|static initializer')
run('static-forward-scalar-rejected','obj','mod t; const u64 A = B; const u64 B = 7;',expect=1,pattern='constant|static initializer')
run('static-self-reference-rejected','obj','mod t; const u64 A = A;',expect=1,pattern='constant|static initializer')

# Source-backed archive version/target mismatches must stop before linking.
archive_dir=work/'archive-ABI'; archive_dir.mkdir()
libsource=archive_dir/'sample.sts'; libsource.write_text('mod sample; ext fn answer(void): i32 { ret 42; }')
archive=archive_dir/'sample.a'; caller=archive_dir/'caller.sts'
caller.write_text('mod caller; lib "sample" from "sample.a"; imp sample; fn main(void): i32 { ret sample.answer() - 42; }')
def archive_abi():
    result=capture([compiler,'lib',str(libsource),'-o',str(archive)],ROOT,archive_dir/'lib-compile',a.timeout)
    assert result['returncode']==0 and archive.is_file(),result['stderr']
    metadata=Path(str(archive)+'.stasha-abi')
    text=metadata.read_text(); assert text.startswith('STASHA_ABI 10\nTARGET ')
    output=archive_dir/'caller'
    result=capture([compiler,'build',str(caller),'-o',str(output)],ROOT,archive_dir/'valid-compile',a.timeout)
    assert result['returncode']==0 and output.is_file(),result['stderr']
    metadata.write_text(text.replace('STASHA_ABI 10','STASHA_ABI 1'))
    result=capture([compiler,'build',str(caller),'-o',str(output)],ROOT,archive_dir/'invalid-compile',a.timeout)
    assert result['returncode']==1 and not output.exists(),result['stderr']
    assert 'incompatible ABI metadata' in result['stderr']
check('archive-ABI-rebuild-gate',archive_abi)

(work/'results.json').write_text(json.dumps(results,indent=2)+'\n')
for result in results:
    print(('FAIL' if result['issues'] else 'PASS')+' '+result['name'])
    for issue in result['issues']: print('  '+issue)
failed=sum(bool(result['issues']) for result in results)
print(f'{len(results)-failed} passed, {failed} failed; artifacts: {work}')
sys.exit(bool(failed))
