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
    def invoke(self, code, stale=False, fail_match=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); tools=root/'tools';tools.mkdir(); output=root/'evidence'
            if stale:
                output.mkdir();(output/'RESULT.txt').write_text('PASS stale')
            compiler=root/'compiler';compiler.write_text('#!/bin/sh\necho mock-compiler\n');compiler.chmod(0o755)
            python=tools/'python3'
            body=f'#!/bin/sh\nexit {code}\n'
            if fail_match is not None:
                body=f'#!/bin/sh\ncase "$1" in {fail_match}) exit {code};; esac\nexit 0\n'
            python.write_text(body);python.chmod(0o755)
            env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],LLVM_BUILD=str(root))
            result=subprocess.run(['bash',str(ROOT/'scripts/qualify_linux.sh'),str(compiler),str(output)],env=env,capture_output=True,text=True,timeout=20)
            return result,(output/'RESULT.txt').read_text() if (output/'RESULT.txt').exists() else None
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
    def test_all_mock_stages_reach_explicit_bounded_receipt(self):
        result,receipt=self.invoke(0)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('bounded Linux hosted compiler qualification',receipt)

if __name__=='__main__': unittest.main()
