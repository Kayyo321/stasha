"""Execute qualification workflow shell commands with no-network tool mocks.

This tests command ordering/cache control flow, not CMake or compiler semantics.
The small reader accepts only the workflow's existing run/name/if scalar subset;
no third-party YAML dependency is needed for the harness selftests.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT/'.github/workflows/qualify-linux.yml'
BOOTSTRAP = 'Build pinned LLVM and LLD'
CLANG = 'Ensure pinned Clang and resource headers'
VERIFY = 'Verify pinned source and build identity'
COMPILER = 'Build compiler'
QUALIFY = 'Run strict hosted compiler qualification'
CACHE_MISS = "steps.llvm-cache.outputs.cache-hit != 'true'"
HOST_CC = 'hostile-ambient-host-cc'
# An executable identity fixture, never a compiler. Fail visibly if a workflow
# regression invokes it instead of the mocked build/qualification commands.
PINNED_DRIVER = '#!'+sys.executable+'\n'+textwrap.dedent('''\
    import json, os, sys
    with open(os.environ['MOCK_TRACE'], 'a') as stream:
        stream.write(json.dumps({'tag': 'pinned-driver-invoked', 'args': sys.argv[1:]})+'\\n')
    raise SystemExit(98)
    ''')


def read_workflow():
    text = WORKFLOW.read_text()
    steps = []
    for block in re.split(r'^      - ',text,flags=re.MULTILINE)[1:]:
        lines = block.splitlines()
        if not lines[0].startswith('name: '):
            continue
        step = {'name':lines[0][len('name: '):]}
        for index,line in enumerate(lines[1:],1):
            if line.startswith('        if: '):
                step['if'] = line[len('        if: '):]
            if line.startswith('        run: '):
                run = line[len('        run: '):]
                if run == '|':
                    body = []
                    for entry in lines[index+1:]:
                        if entry and not entry.startswith('          '):break
                        body.append(entry)
                    run = textwrap.dedent('\n'.join(body))
                step['run'] = run
        steps.append(step)
    return text,steps


@unittest.skipUnless(os.name=='posix','Workflow shell mocks require POSIX Bash')
class QualificationToolchainWorkflowTests(unittest.TestCase):
    def invoke(self, cache_hit, fail='', cached_clang='valid', build_path='relative',
               mutation='', mutate_after='verify', run_overrides=None):
        text,steps = read_workflow()
        names = [step['name'] for step in steps]
        selected = steps[names.index(BOOTSTRAP):names.index(QUALIFY)+1]
        self.assertEqual([step['name'] for step in selected],[BOOTSTRAP,CLANG,VERIFY,COMPILER,QUALIFY])
        for step in selected[:2]:self.assertEqual(step.get('if'),CACHE_MISS)
        for step in selected[2:]:self.assertNotIn('if',step)
        env_block = text.split('    env:\n',1)[1].split('    steps:\n',1)[0]
        workflow_env = {}
        for line in env_block.splitlines():
            key,value=line.strip().split(': ',1)
            workflow_env[key]=value.strip("'\"")
        self.assertEqual(workflow_env['CC'],'clang')
        self.assertEqual(workflow_env['CXX'],'clang++')
        self.assertEqual(workflow_env['LLVM_ENABLE_PROJECTS'],'lld;clang')
        self.assertEqual(workflow_env['LLVM_BUILD'],'build/llvm')
        self.assertNotIn('HOST_CC',workflow_env)
        self.assertIn('key: llvm-qualification-v2-${{ steps.inputs.outputs.key }}',text)
        self.assertNotIn('restore-keys:',text)
        frontend = steps[names.index('Run frontend and oracle regressions')]
        self.assertEqual(frontend['run'].strip(),
                         'bash scripts/test_harness_selftest.sh\nbash scripts/test_frontend.sh')
        self.assertLess(names.index(frontend['name']),names.index(BOOTSTRAP))
        fingerprint = steps[names.index('Fingerprint exact toolchain inputs')]
        self.assertEqual(fingerprint['run'],
                         'python3 scripts/toolchain_fingerprint.py --github-output --output build/toolchain-inputs.json')
        self.assertLess(names.index(frontend['name']),names.index(fingerprint['name']))
        self.assertLess(names.index(fingerprint['name']),names.index(BOOTSTRAP))
        self.assertNotIn('GITHUB_ENV',text)
        self.assertNotIn('GITHUB_PATH',text)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);tools=root/'tools';tools.mkdir();trace=root/'trace.jsonl'
            self.assertIn(build_path,('relative','absolute','symlink'))
            if build_path == 'absolute':
                workflow_env['LLVM_BUILD']=str(root/'absolute-llvm')
            elif build_path == 'symlink':
                physical=root/'physical-llvm';physical.mkdir()
                link=root/workflow_env['LLVM_BUILD'];link.parent.mkdir(parents=True)
                link.symlink_to(physical,target_is_directory=True)
            build=root/workflow_env['LLVM_BUILD']
            self.workflow_build=workflow_env['LLVM_BUILD']
            self.pinned_cc=str(build.resolve()/'bin/clang')
            self.pinned_cxx=str(build.resolve()/'bin/clang++')
            self.mock_cwd=str(root.resolve())
            if cache_hit == 'true':
                self.assertIn(cached_clang,('valid','missing','invalid','missing-resource'))
                clang=build/'bin/clang';clang.parent.mkdir(parents=True,exist_ok=True)
                if cached_clang != 'missing':
                    clang.write_text(PINNED_DRIVER if cached_clang != 'invalid' else 'invalid cached Clang')
                    clang.chmod(0o755)
                (clang.parent/'clang++').symlink_to('clang')
                resource=build/'lib/clang/23/include';resource.mkdir(parents=True)
                for name in ('stdint.h','stddef.h','stdarg.h'):
                    if cached_clang != 'missing-resource' or name != 'stdint.h':
                        (resource/name).write_text('mock resource header')
            stub = '#!'+sys.executable+'\n'+textwrap.dedent('''\
                import json, os, shutil
                from pathlib import Path
                import sys
                name = Path(sys.argv[0]).name
                args = sys.argv[1:]
                if name == 'make': tag = 'bootstrap' if args[0] == 'llvm' else 'compiler'
                elif name == 'cmake': tag = 'clang-build' if args[0] == '--build' else 'clang-configure'
                elif name == 'python3': tag = 'verify'
                elif name == 'bash': tag = 'qualify'
                elif name in ('clang', 'clang++'): tag = 'host-'+name
                else: raise SystemExit('unexpected mocked tool: '+name)
                with open(os.environ['MOCK_TRACE'],'a') as stream:
                    stream.write(json.dumps({'tag':tag,'args':args,'cwd':os.getcwd(),
                        'projects':os.environ['LLVM_ENABLE_PROJECTS'],
                        'CC':os.environ.get('CC'),'CXX':os.environ.get('CXX'),
                        'HOST_CC':os.environ.get('HOST_CC')})+'\\n')
                if tag.startswith('host-'): raise SystemExit(97)
                if tag == os.environ['MOCK_FAIL']: raise SystemExit(73)
                # Model a complete cached/build-produced toolchain. Detailed
                # identity/resource rejection belongs to verifier unit tests.
                build = Path(os.environ['LLVM_BUILD']).resolve()
                clang = build/'bin/clang'
                cxx = build/'bin/clang++'
                resource = build/'lib/clang/23/include'
                driver = os.environ['MOCK_PINNED_DRIVER']
                if tag == 'clang-build':
                    clang.parent.mkdir(parents=True,exist_ok=True)
                    clang.write_text(driver); clang.chmod(0o755)
                    cxx.symlink_to('clang')
                    resource.mkdir(parents=True,exist_ok=True)
                    for name in ('stdint.h','stddef.h','stdarg.h'):
                        (resource/name).write_text('mock resource header')
                if tag == 'verify':
                    if not clang.is_file() or clang.read_text() != driver: raise SystemExit(74)
                    if not os.access(clang,os.X_OK): raise SystemExit(74)
                    if any(not (resource/name).is_file() for name in ('stdint.h','stddef.h','stdarg.h')): raise SystemExit(74)
                if tag == 'compiler':
                    # Makefile assignments override environment variables, so
                    # a pinned-looking environment cannot satisfy this check.
                    if args != ['-j2','CC='+str(clang),'CXX='+str(cxx),
                                'STASHA_NO_OPENSSL=1','all',
                                'tests/cinterop/build/libcinterop_support.a']:
                        raise SystemExit(75)
                if tag == 'qualify':
                    if (os.environ.get('CC'),os.environ.get('CXX'),os.environ.get('HOST_CC')) != (str(clang),str(cxx),'gcc'):
                        raise SystemExit(76)
                if tag == os.environ['MOCK_MUTATE_AFTER']:
                    mutation = os.environ['MOCK_MUTATION']
                    if mutation == 'missing-build': shutil.rmtree(build)
                    elif mutation == 'missing-clang': clang.unlink()
                    elif mutation == 'nonexecutable-clang': clang.chmod(0o644)
                    elif mutation in ('missing-clang++','dangling-clang++',
                                      'nonexecutable-clang++','foreign-clang++'):
                        cxx.unlink()
                        if mutation == 'dangling-clang++': cxx.symlink_to('absent-driver')
                        elif mutation != 'missing-clang++':
                            cxx.write_text(driver)
                            cxx.chmod(0o644 if mutation == 'nonexecutable-clang++' else 0o755)
                    elif mutation: raise SystemExit('unexpected mutation: '+mutation)
                ''')
            for name in ('make','cmake','python3','bash','clang','clang++'):
                tool=tools/name;tool.write_text(stub);tool.chmod(0o755)
            env=dict(os.environ,**workflow_env)
            env.update(PATH=str(tools),HOST_CC=HOST_CC,MOCK_TRACE=str(trace),MOCK_FAIL=fail,
                       MOCK_PINNED_DRIVER=PINNED_DRIVER,MOCK_MUTATION=mutation,
                       MOCK_MUTATE_AFTER=mutate_after)
            stdout=[];stderr=[]
            self.step_names=[]
            for step in selected:
                # actions/cache emits 'true' only for an exact primary-key hit.
                # A nonexact hit emits 'false'; an absent output also builds.
                if 'if' in step and cache_hit == 'true':continue
                # GitHub starts a fresh shell per step. Never carry shell-local
                # variables or exports from the build into qualification.
                body=(run_overrides or {}).get(step['name'],step['run'])
                result=subprocess.run(['/bin/bash','-euo','pipefail','-c',body],
                                      cwd=root,env=env,capture_output=True,text=True,timeout=20)
                self.step_names.append(step['name'])
                stdout.append(result.stdout);stderr.append(result.stderr)
                if result.returncode:break
            result=subprocess.CompletedProcess(result.args,result.returncode,''.join(stdout),''.join(stderr))
            records=[json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            return result,records

    def assert_commands(self,records,cache_hit):
        expected=['verify','compiler','qualify']
        if cache_hit != 'true':expected[:0]=['bootstrap','clang-configure','clang-build']
        self.assertEqual([r['tag'] for r in records],expected)
        by_tag={r['tag']:r for r in records}
        self.assertTrue(all(r['projects']=='lld;clang' for r in records))
        self.assertTrue(all(r['cwd']==self.mock_cwd for r in records))
        for record in records:
            expected_env=(self.pinned_cc,self.pinned_cxx,'gcc') if record['tag']=='qualify' else ('clang','clang++',HOST_CC)
            self.assertEqual((record['CC'],record['CXX'],record['HOST_CC']),expected_env)
        if cache_hit != 'true':
            self.assertEqual(by_tag['bootstrap']['args'],['llvm','LLVM_BUILD_JOBS=2','LLVM_LINK_JOBS=1'])
            self.assertEqual(by_tag['clang-configure']['args'],[
                '-S','extlib/llvm/llvm','-B',self.workflow_build,'-DLLVM_ENABLE_PROJECTS=lld;clang',
                '-DLLVM_APPEND_VC_REV=ON','-DLLVM_PARALLEL_LINK_JOBS=1'])
            self.assertEqual(by_tag['clang-build']['args'],['--build',self.workflow_build,'--target','clang','--parallel','2'])
        self.assertEqual(by_tag['verify']['args'],[
            'scripts/verify_llvm_toolchain.py','--llvm-build',self.workflow_build,'--require-clang',
            '--output','build/qualification/toolchain.json'])
        self.assertEqual(by_tag['compiler']['args'],[
            '-j2','CC='+self.pinned_cc,'CXX='+self.pinned_cxx,'STASHA_NO_OPENSSL=1',
            'all','tests/cinterop/build/libcinterop_support.a'])
        self.assertEqual(by_tag['qualify']['args'],['scripts/qualify_linux.sh','./bin/stasha','build/qualification'])
        self.assertTrue(Path(self.pinned_cc).is_absolute())
        self.assertTrue(self.pinned_cxx.endswith('/bin/clang++'))
        expected_steps=[VERIFY,COMPILER,QUALIFY]
        if cache_hit != 'true':expected_steps[:0]=[BOOTSTRAP,CLANG]
        self.assertEqual(self.step_names,expected_steps)

    def test_cold_cache_bootstraps_then_builds_and_verifies_clang(self):
        result,records=self.invoke('')
        self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,'')

    def test_exact_warm_cache_skips_builds_but_still_verifies_and_qualifies(self):
        result,records=self.invoke('true')
        self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,'true')
        self.assertIn(f'Stasha build CC={self.pinned_cc} CXX={self.pinned_cxx}\n',result.stdout)
        self.assertIn(f'Hosted qualification CC={self.pinned_cc} CXX={self.pinned_cxx} HOST_CC=gcc\n',result.stdout)

    def test_nonexact_or_missing_cache_hit_output_builds_and_verifies(self):
        for cache_hit in ('false',None):
            with self.subTest(cache_hit=cache_hit):
                result,records=self.invoke(cache_hit)
                self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,cache_hit)

    def test_absolute_and_symlink_build_paths_select_physical_driver_pair(self):
        for build_path in ('absolute','symlink'):
            for cache_hit in ('','true'):
                with self.subTest(build_path=build_path,cache_hit=cache_hit):
                    result,records=self.invoke(cache_hit,build_path=build_path)
                    self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,cache_hit)
                    if build_path == 'symlink':
                        self.assertIn('/physical-llvm/bin/clang',self.pinned_cc)

    def test_invalid_exact_hit_fails_before_compiler_or_qualification(self):
        for cached_clang in ('missing','invalid','missing-resource'):
            with self.subTest(cached_clang=cached_clang):
                result,records=self.invoke('true',cached_clang=cached_clang)
                self.assertEqual(result.returncode,74,result.stderr)
                self.assertEqual([r['tag'] for r in records],['verify'])
                self.assertEqual(self.step_names,[VERIFY])

    def test_toolchain_failure_prevents_compiler_build_and_qualification(self):
        for cache_hit in ('','false',None,'true'):
            stages=['verify']
            if cache_hit != 'true':stages[:0]=['bootstrap','clang-configure','clang-build']
            for failure in stages:
                with self.subTest(cache_hit=cache_hit,failure=failure):
                    result,records=self.invoke(cache_hit,failure)
                    self.assertEqual(result.returncode,73,result.stderr)
                    self.assertEqual([r['tag'] for r in records],stages[:stages.index(failure)+1])

    def test_bad_driver_pair_or_removed_build_fails_both_step_guards(self):
        mutations=('missing-clang','nonexecutable-clang','missing-clang++',
                   'dangling-clang++','nonexecutable-clang++','foreign-clang++','missing-build')
        for cache_hit in ('','true'):
            for mutate_after in ('verify','compiler'):
                for mutation in mutations:
                    with self.subTest(cache_hit=cache_hit,mutate_after=mutate_after,mutation=mutation):
                        result,records=self.invoke(cache_hit,mutation=mutation,mutate_after=mutate_after)
                        self.assertEqual(result.returncode,1,result.stderr)
                        expected=['verify']
                        if cache_hit != 'true':expected[:0]=['bootstrap','clang-configure','clang-build']
                        if mutate_after == 'compiler':expected.append('compiler')
                        self.assertEqual([r['tag'] for r in records],expected)
                        self.assertEqual(self.step_names[-1],COMPILER if mutate_after=='verify' else QUALIFY)
                        if mutation != 'missing-build':
                            self.assertIn('Pinned clang/clang++ driver pair is missing or inconsistent',result.stderr)

    def test_compiler_and_qualification_failures_propagate(self):
        for cache_hit in ('','true'):
            for failure in ('compiler','qualify'):
                with self.subTest(cache_hit=cache_hit,failure=failure):
                    result,records=self.invoke(cache_hit,failure)
                    self.assertEqual(result.returncode,73,result.stderr)
                    expected=['verify','compiler']
                    if cache_hit != 'true':expected[:0]=['bootstrap','clang-configure','clang-build']
                    if failure == 'qualify':expected.append('qualify')
                    self.assertEqual([r['tag'] for r in records],expected)
                    self.assertEqual(self.step_names[-1],COMPILER if failure=='compiler' else QUALIFY)

    def test_make_mock_rejects_environment_only_compiler_selection(self):
        environment_only='''llvm_build="$(cd -- "$LLVM_BUILD" && pwd -P)"
CC="$llvm_build/bin/clang" CXX="$llvm_build/bin/clang++" make -j2 STASHA_NO_OPENSSL=1 all tests/cinterop/build/libcinterop_support.a'''
        result,records=self.invoke('true',run_overrides={COMPILER:environment_only})
        self.assertEqual(result.returncode,75,result.stderr)
        self.assertEqual([r['tag'] for r in records],['verify','compiler'])
        self.assertEqual((records[-1]['CC'],records[-1]['CXX']),(self.pinned_cc,self.pinned_cxx))
        self.assertEqual(self.step_names,[VERIFY,COMPILER])

    def test_qualification_preflight_requires_clang(self):
        script=(ROOT/'scripts/qualify_linux.sh').read_text()
        command=next(line for line in script.splitlines() if line.startswith('python3 scripts/verify_llvm_toolchain.py '))
        self.assertIn('--require-clang',shlex.split(command))


if __name__ == '__main__':unittest.main()
