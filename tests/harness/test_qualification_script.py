"""Control-flow/oracle tests only; mocked commands do not qualify a compiler."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
@unittest.skipUnless(platform.system()=='Linux' and platform.machine()=='x86_64','Linux qualification shell control-flow tests')
class QualificationShellTests(unittest.TestCase):
    def invoke(self, code, stale=False, fail_match=None, match_args=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); tools=root/'tools';tools.mkdir(); output=root/'evidence'
            if stale:
                output.mkdir();(output/'RESULT.txt').write_text('PASS stale')
            trace=root/'commands.log'
            compiler=root/'compiler';compiler.write_text('#!/bin/sh\nprintf "compiler %s\\n" "$*" >> "$MOCK_TRACE"\necho mock-compiler\n');compiler.chmod(0o755)
            python=tools/'python3'
            preamble='#!/bin/sh\nprintf "python3 %s\\n" "$*" >> "$MOCK_TRACE"\n'
            body=preamble+f'exit {code}\n'
            if fail_match is not None:
                match_value='"$*"' if match_args else '"$1"'
                body=preamble+f'case {match_value} in {fail_match}) exit {code};; esac\nexit 0\n'
            python.write_text(body);python.chmod(0o755)
            env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],PYTHON=str(python),LLVM_BUILD=str(root),MOCK_TRACE=str(trace))
            result=subprocess.run(['bash',str(ROOT/'scripts/qualify_linux.sh'),str(compiler),str(output)],env=env,capture_output=True,text=True,timeout=20)
            self.commands=trace.read_text().splitlines() if trace.exists() else []
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
    def test_all_mock_stages_reach_explicit_bounded_receipt(self):
        result,receipt=self.invoke(0)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('bounded Linux hosted compiler qualification',receipt)
        self.assertIn(' --require-clang --output ',self.commands[0])
        self.assertEqual(self.commands[1],'compiler --version')
        self.assertIn('python3 -m unittest discover -s tests/driver -p test_project_cli.py',self.commands)
        self.assertEqual(sum(command.startswith('python3 tests/driver/project_cli.py --compiler ')
                             for command in self.commands),1)

if __name__=='__main__': unittest.main()
