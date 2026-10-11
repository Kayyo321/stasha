"""Control-flow/oracle tests only; mocked commands do not qualify a compiler."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT=Path(__file__).resolve().parents[2]
@unittest.skipUnless(platform.system()=='Linux' and platform.machine()=='x86_64','Linux qualification shell control-flow tests')
class QualificationShellTests(unittest.TestCase):
    def invoke(self, code, stale=False, fail_match=None, match_args=False, compiler_env=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); tools=root/'tools';tools.mkdir(); output=root/'evidence'
            if stale:
                output.mkdir();(output/'RESULT.txt').write_text('PASS stale')
            trace=root/'commands.log'
            environment_trace=root/'environments.jsonl'
            compiler=root/'compiler';compiler.write_text('#!/bin/sh\nprintf "compiler %s\\n" "$*" >> "$MOCK_TRACE"\necho mock-compiler\n');compiler.chmod(0o755)
            python=tools/'python3'
            body='#!'+sys.executable+'\n'+textwrap.dedent('''\
                import fnmatch, json, os, sys
                args=sys.argv[1:]
                with open(os.environ['MOCK_TRACE'],'a') as stream:
                    stream.write('python3 '+' '.join(args)+'\\n')
                with open(os.environ['MOCK_ENV_TRACE'],'a') as stream:
                    stream.write(json.dumps({'args':args,'cwd':os.getcwd(),
                        'CC':os.environ.get('CC'),'CXX':os.environ.get('CXX'),
                        'HOST_CC':os.environ.get('HOST_CC')})+'\\n')
                pattern=os.environ.get('MOCK_FAIL_MATCH')
                value=' '.join(args) if os.environ['MOCK_MATCH_ARGS']=='1' else args[0]
                if pattern is None or fnmatch.fnmatchcase(value,pattern):
                    raise SystemExit(int(os.environ['MOCK_CODE']))
                ''')
            python.write_text(body);python.chmod(0o755)
            env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],PYTHON=str(python),LLVM_BUILD=str(root),MOCK_TRACE=str(trace),
                     MOCK_ENV_TRACE=str(environment_trace),MOCK_CODE=str(code),MOCK_MATCH_ARGS='1' if match_args else '0')
            env.pop('MOCK_FAIL_MATCH',None)
            if fail_match is not None:env['MOCK_FAIL_MATCH']=fail_match.strip("'\"")
            env.update(compiler_env or {})
            result=subprocess.run(['bash',str(ROOT/'scripts/qualify_linux.sh'),str(compiler),str(output)],env=env,capture_output=True,text=True,timeout=20)
            self.commands=trace.read_text().splitlines() if trace.exists() else []
            self.command_records=[json.loads(line) for line in environment_trace.read_text().splitlines()] if environment_trace.exists() else []
            self.status=(output/'STATUS.txt').read_text() if (output/'STATUS.txt').exists() else None
            return result,(output/'RESULT.txt').read_text() if (output/'RESULT.txt').exists() else None
    def test_clang_preflight_failure_stops_before_compiler_or_suites(self):
        result,receipt=self.invoke(84,stale=True,fail_match='scripts/verify_llvm_toolchain.py')
        self.assertEqual(result.returncode,84,result.stderr);self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=84\n')
        self.assertEqual(len(self.commands),1,self.commands)
        self.assertTrue(self.commands[0].startswith('python3 scripts/verify_llvm_toolchain.py --llvm-build '))
        self.assertIn(' --require-clang --output ',self.commands[0])
    def test_failed_stage_cannot_publish_pass(self):
        result,receipt=self.invoke(73)
        self.assertEqual(result.returncode,73,result.stderr); self.assertIsNone(receipt)
    def test_failed_rerun_removes_previous_pass(self):
        result,receipt=self.invoke(73,stale=True)
        self.assertEqual(result.returncode,73,result.stderr); self.assertIsNone(receipt)
    def test_bounds_stage_failure_blocks_receipt(self):
        result,receipt=self.invoke(74,stale=True,fail_match='tests/memory/run_bounds_a1.py')
        self.assertEqual(result.returncode,74,result.stderr); self.assertIsNone(receipt)
    def test_constructor_stage_failure_blocks_receipt(self):
        result,receipt=self.invoke(75,stale=True,fail_match='tests/memory/run_make_b1.py')
        self.assertEqual(result.returncode,75,result.stderr); self.assertIsNone(receipt)
    def test_interface_stage_failure_blocks_receipt(self):
        result,receipt=self.invoke(76,stale=True,fail_match='tests/interfaces/run_i0.py')
        self.assertEqual(result.returncode,76,result.stderr); self.assertIsNone(receipt)
    def test_interface_binding_failure_blocks_receipt(self):
        result,receipt=self.invoke(77,stale=True,fail_match='tests/interfaces/run_i1a.py')
        self.assertEqual(result.returncode,77,result.stderr); self.assertIsNone(receipt)
    def test_interface_source_call_failure_blocks_receipt(self):
        result,receipt=self.invoke(78,stale=True,fail_match='tests/interfaces/run_i1b.py')
        self.assertEqual(result.returncode,78,result.stderr); self.assertIsNone(receipt)
    def test_interface_local_transfer_failure_blocks_receipt(self):
        result,receipt=self.invoke(79,stale=True,fail_match='tests/interfaces/i1b2_run.py')
        self.assertEqual(result.returncode,79,result.stderr); self.assertIsNone(receipt)
    def test_array_query_failure_blocks_receipt(self):
        result,receipt=self.invoke(80,stale=True,fail_match='tests/memory/run_array_queries.py')
        self.assertEqual(result.returncode,80,result.stderr); self.assertIsNone(receipt)
    def test_nominal_boundary_failure_blocks_receipt(self):
        result,receipt=self.invoke(81,stale=True,fail_match='tests/interfaces/i1b3a_run.py')
        self.assertEqual(result.returncode,81,result.stderr); self.assertIsNone(receipt)
    def test_factory_result_failure_blocks_receipt(self):
        result,receipt=self.invoke(82,stale=True,fail_match='tests/interfaces/factory_result_run.py')
        self.assertEqual(result.returncode,82,result.stderr); self.assertIsNone(receipt)
    def test_descriptor_default_failure_blocks_receipt(self):
        result,receipt=self.invoke(83,stale=True,fail_match='tests/interfaces/descriptor_defaults_run.py')
        self.assertEqual(result.returncode,83,result.stderr); self.assertIsNone(receipt)
    def test_project_cli_failure_blocks_receipt_and_later_stages(self):
        result,receipt=self.invoke(85,stale=True,fail_match='tests/driver/project_cli.py')
        self.assertEqual(result.returncode,85,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=85\n')
        self.assertTrue(self.commands[-1].startswith('python3 tests/driver/project_cli.py --compiler '))
        self.assertIn(' --artifacts ',self.commands[-1])
        self.assertTrue(self.commands[-1].endswith('/project-cli'))
        self.assertFalse(any('tests/driver/global_initializers.py' in command for command in self.commands))
    def test_project_cli_mock_control_failure_stops_real_suites(self):
        result,receipt=self.invoke(86,stale=True,match_args=True,
                                   fail_match="'-m unittest discover -s tests/driver -p test_project_cli.py'")
        self.assertEqual(result.returncode,86,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=86\n')
        self.assertEqual(self.commands[-1],'python3 -m unittest discover -s tests/driver -p test_project_cli.py')
        self.assertFalse(any('tests/harness/compiler_suites.py' in command for command in self.commands))
    def test_editor_coordinate_failure_blocks_receipt_and_later_stages(self):
        result,receipt=self.invoke(87,stale=True,fail_match='tests/driver/editor_coordinates.py')
        self.assertEqual(result.returncode,87,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=87\n')
        self.assertTrue(self.commands[-1].startswith('python3 tests/driver/editor_coordinates.py --compiler '))
        self.assertIn(' --artifacts ',self.commands[-1])
        self.assertTrue(self.commands[-1].endswith('/editor-coordinates'))
        self.assertFalse(any('tests/driver/global_initializers.py' in command for command in self.commands))
    def test_editor_coordinate_mock_control_failure_stops_real_suites(self):
        result,receipt=self.invoke(88,stale=True,match_args=True,
                                   fail_match="'-m unittest discover -s tests/driver -p test_editor_coordinates.py'")
        self.assertEqual(result.returncode,88,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=88\n')
        self.assertEqual(self.commands[-1],'python3 -m unittest discover -s tests/driver -p test_editor_coordinates.py')
        self.assertFalse(any('tests/harness/compiler_suites.py' in command for command in self.commands))
    def test_format_precision_failure_blocks_receipt_and_later_stages(self):
        result,receipt=self.invoke(89,stale=True,fail_match='tests/format/run_format_tests.py')
        self.assertEqual(result.returncode,89,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=89\n')
        self.assertTrue(self.commands[-1].startswith('python3 tests/format/run_format_tests.py --compiler '))
        self.assertIn(' --helper-cc ',self.commands[-1])
        self.assertIn(' --phase all --artifacts ',self.commands[-1])
        self.assertTrue(self.commands[-1].endswith('/format-precision'))
        self.assertFalse(any('tests/layout/run_layout_tests.py' in command for command in self.commands))
    def test_format_precision_mock_control_failure_stops_real_suites(self):
        result,receipt=self.invoke(90,stale=True,match_args=True,
                                   fail_match="'-m unittest discover -s tests/format -p test_run_format_tests.py'")
        self.assertEqual(result.returncode,90,result.stderr); self.assertIsNone(receipt)
        self.assertEqual(self.status,'FAIL exit=90\n')
        self.assertEqual(self.commands[-1],'python3 -m unittest discover -s tests/format -p test_run_format_tests.py')
        self.assertFalse(any('tests/harness/compiler_suites.py' in command for command in self.commands))
    def test_all_mock_stages_reach_explicit_bounded_receipt(self):
        result,receipt=self.invoke(0)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('bounded Linux hosted compiler qualification',receipt)
        self.assertIn('python3 -m unittest discover -s tests/format -p test_run_format_tests.py',self.commands)
        format_steps=[i for i,command in enumerate(self.commands)
                      if command.startswith('python3 tests/format/run_format_tests.py --compiler ')]
        self.assertEqual(len(format_steps),1)
        self.assertIn(' --cc ',self.commands[format_steps[0]])
        self.assertIn(' --helper-cc ',self.commands[format_steps[0]])
        layout_step=next(i for i,command in enumerate(self.commands)
                         if command.startswith('python3 tests/layout/run_layout_tests.py'))
        self.assertLess(format_steps[0],layout_step)
        self.assertIn(' --require-clang --output ',self.commands[0])
        self.assertEqual(self.commands[1],'compiler --version')
        self.assertIn('python3 -m unittest discover -s tests/driver -p test_project_cli.py',self.commands)
        self.assertEqual(sum(command.startswith('python3 tests/driver/project_cli.py --compiler ')
                             for command in self.commands),1)
        self.assertIn('python3 -m unittest discover -s tests/driver -p test_editor_coordinates.py',self.commands)
        self.assertEqual(sum(command.startswith('python3 tests/driver/editor_coordinates.py --compiler ')
                             for command in self.commands),1)

    def test_pinned_environment_routes_native_and_inherited_compiler_roles(self):
        selection={'CC':'/mock-pinned/llvm/bin/clang','CXX':'/mock-pinned/llvm/bin/clang++','HOST_CC':'gcc'}
        result,receipt=self.invoke(0,stale=True,compiler_env=selection)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(receipt,'PASS: bounded Linux hosted compiler qualification; see native/kernel qualification limits.\n')
        self.assertTrue(self.status.startswith('PASS source='),self.status)
        self.assertEqual(self.command_records[0]['args'][0],'scripts/verify_llvm_toolchain.py')
        self.assertIn('--require-clang',self.command_records[0]['args'])
        self.assertEqual(self.commands[1],'compiler --version')
        for record in self.command_records:
            self.assertEqual({name:record[name] for name in selection},selection)
            self.assertEqual(record['cwd'],str(ROOT))

        def command(script):
            matches=[record['args'] for record in self.command_records if record['args'][0]==script]
            self.assertEqual(len(matches),1,script)
            return matches[0]

        native_scripts=('tests/memory/run_bounds_a1.py','tests/memory/run_make_b1.py',
                        'tests/format/run_format_tests.py','tests/layout/run_layout_tests.py',
                        'tests/abi/run_sysv_aggregates.py','tests/abi/run_c_callbacks.py',
                        'tests/abi/run_c_callback_parameters.py','tests/abi/run_member_pointers.py')
        for script in native_scripts:
            args=command(script)
            self.assertEqual(args[args.index('--cc')+1],selection['CC'],script)
        args=command('tests/format/run_format_tests.py')
        self.assertEqual(args[args.index('--helper-cc')+1],'gcc')
        self.assertIn('--build-trap-hook',command('tests/scalars/run.py'))
        self.assertIn('--zone-probe',command('tests/cleanup/run.py'))
        self.assertIn('--compiler',command('tests/driver/global_initializers.py'))
        interop=[record for record in self.command_records
                 if record['args'][:2]==['tests/harness/compiler_suites.py','cinterop']]
        self.assertEqual(len(interop),1)
        self.assertEqual(interop[0]['CXX'],selection['CXX'])

if __name__=='__main__': unittest.main()
