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
    def invoke(self, code, stale=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); tools=root/'tools';tools.mkdir(); output=root/'evidence'
            if stale:
                output.mkdir();(output/'RESULT.txt').write_text('PASS stale')
            compiler=root/'compiler';compiler.write_text('#!/bin/sh\necho mock-compiler\n');compiler.chmod(0o755)
            python=tools/'python3';python.write_text(f'#!/bin/sh\nexit {code}\n');python.chmod(0o755)
            env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],LLVM_BUILD=str(root))
            result=subprocess.run(['bash',str(ROOT/'scripts/qualify_linux.sh'),str(compiler),str(output)],env=env,capture_output=True,text=True,timeout=20)
            return result,(output/'RESULT.txt').read_text() if (output/'RESULT.txt').exists() else None
    def test_failed_stage_cannot_publish_pass(self):
        result,receipt=self.invoke(73)
        self.assertEqual(result.returncode,73,result.stderr); self.assertIsNone(receipt)
    def test_failed_rerun_removes_previous_pass(self):
        result,receipt=self.invoke(73,stale=True)
        self.assertEqual(result.returncode,73,result.stderr); self.assertIsNone(receipt)
    def test_all_mock_stages_reach_explicit_bounded_receipt(self):
        result,receipt=self.invoke(0)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('bounded Linux hosted compiler qualification',receipt)

if __name__=='__main__': unittest.main()
