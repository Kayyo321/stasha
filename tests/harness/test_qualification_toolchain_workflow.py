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
    def invoke(self, cache_hit, fail=''):
        text,steps = read_workflow()
        names = [step['name'] for step in steps]
        selected = steps[names.index(BOOTSTRAP):names.index(QUALIFY)+1]
        self.assertEqual([step['name'] for step in selected],[BOOTSTRAP,CLANG,VERIFY,COMPILER,QUALIFY])
        self.assertEqual(selected[0].get('if'),CACHE_MISS)
        for step in selected[1:]:self.assertNotIn('if',step)
        env_block = text.split('    env:\n',1)[1].split('    steps:\n',1)[0]
        workflow_env = {}
        for line in env_block.splitlines():
            key,value=line.strip().split(': ',1)
            workflow_env[key]=value.strip("'\"")
        self.assertEqual(workflow_env['LLVM_ENABLE_PROJECTS'],'lld;clang')
        self.assertEqual(workflow_env['LLVM_BUILD'],'build/llvm')
        self.assertIn('key: llvm-qualification-v2-${{ steps.inputs.outputs.key }}',text)
        self.assertNotIn('restore-keys:',text)
        script = ['set -euo pipefail']
        for step in selected:
            if 'if' in step and cache_hit:continue
            script.append(step['run'])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);tools=root/'tools';tools.mkdir();trace=root/'trace.jsonl'
            stub = '#!'+sys.executable+'\n'+textwrap.dedent('''\
                import json, os
                from pathlib import Path
                import sys
                name = Path(sys.argv[0]).name
                args = sys.argv[1:]
                if name == 'make': tag = 'bootstrap' if args[0] == 'llvm' else 'compiler'
                elif name == 'cmake': tag = 'clang-build' if args[0] == '--build' else 'clang-configure'
                elif name == 'python3': tag = 'verify'
                elif name == 'bash': tag = 'qualify'
                else: raise SystemExit('unexpected mocked tool: '+name)
                with open(os.environ['MOCK_TRACE'],'a') as stream:
                    stream.write(json.dumps({'tag':tag,'args':args,'projects':os.environ['LLVM_ENABLE_PROJECTS']})+'\\n')
                if tag == os.environ['MOCK_FAIL']: raise SystemExit(73)
                # Even a warm cache begins without Clang in these fixtures.
                # The build step must run before verification can continue.
                clang = Path(os.environ['LLVM_BUILD'])/'bin/clang'
                if tag == 'clang-build':
                    clang.parent.mkdir(parents=True,exist_ok=True)
                    clang.write_text('mock built Clang')
                if tag == 'verify' and not clang.is_file(): raise SystemExit(74)
                ''')
            for name in ('make','cmake','python3','bash'):
                tool=tools/name;tool.write_text(stub);tool.chmod(0o755)
            env=dict(os.environ,**workflow_env)
            env.update(PATH=str(tools)+os.pathsep+os.environ['PATH'],MOCK_TRACE=str(trace),MOCK_FAIL=fail)
            result=subprocess.run(['/bin/bash','-c','\n'.join(script)],cwd=root,env=env,capture_output=True,text=True,timeout=20)
            records=[json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            return result,records

    def assert_commands(self,records,cache_hit):
        expected=['clang-configure','clang-build','verify','compiler','qualify']
        if not cache_hit:expected.insert(0,'bootstrap')
        self.assertEqual([r['tag'] for r in records],expected)
        by_tag={r['tag']:r for r in records}
        self.assertTrue(all(r['projects']=='lld;clang' for r in records))
        if not cache_hit:self.assertEqual(by_tag['bootstrap']['args'],['llvm','LLVM_BUILD_JOBS=2','LLVM_LINK_JOBS=1'])
        self.assertEqual(by_tag['clang-configure']['args'],[
            '-S','extlib/llvm/llvm','-B','build/llvm','-DLLVM_ENABLE_PROJECTS=lld;clang',
            '-DLLVM_APPEND_VC_REV=ON','-DLLVM_PARALLEL_LINK_JOBS=1'])
        self.assertEqual(by_tag['clang-build']['args'],['--build','build/llvm','--target','clang','--parallel','2'])
        self.assertEqual(by_tag['verify']['args'],[
            'scripts/verify_llvm_toolchain.py','--llvm-build','build/llvm','--require-clang',
            '--output','build/qualification/toolchain.json'])
        self.assertEqual(by_tag['compiler']['args'],['-j2','STASHA_NO_OPENSSL=1','all','tests/cinterop/build/libcinterop_support.a'])
        self.assertEqual(by_tag['qualify']['args'],['scripts/qualify_linux.sh','./bin/stasha','build/qualification'])

    def test_cold_cache_bootstraps_then_builds_and_verifies_clang(self):
        result,records=self.invoke(False)
        self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,False)

    def test_warm_cache_still_configures_and_builds_missing_clang(self):
        result,records=self.invoke(True)
        self.assertEqual(result.returncode,0,result.stderr);self.assert_commands(records,True)

    def test_toolchain_failure_prevents_compiler_build_and_qualification(self):
        for cache_hit in (False,True):
            stages=['clang-configure','clang-build','verify']
            if not cache_hit:stages.insert(0,'bootstrap')
            for failure in stages:
                with self.subTest(cache_hit=cache_hit,failure=failure):
                    result,records=self.invoke(cache_hit,failure)
                    self.assertEqual(result.returncode,73,result.stderr)
                    self.assertEqual([r['tag'] for r in records],stages[:stages.index(failure)+1])

    def test_qualification_preflight_requires_clang(self):
        script=(ROOT/'scripts/qualify_linux.sh').read_text()
        command=next(line for line in script.splitlines() if line.startswith('python3 scripts/verify_llvm_toolchain.py '))
        self.assertIn('--require-clang',shlex.split(command))


if __name__ == '__main__':unittest.main()
