#!/usr/bin/env python3
"""Fail-closed self-tests. Only fake compiler/benign sentinel programs execute."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from test_i0_archives import archive_rejection_issues
spec=importlib.util.spec_from_file_location('run_i0',Path(__file__).with_name('run_i0.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class RunnerContracts(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='i0-runner-selftest-');self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def run_fake(self,body,*,negative=True,timeout=5,stale=False):
        fake=self.root/'compiler';fake.write_text('#!/usr/bin/env python3\nimport os,sys,time\nfrom pathlib import Path\nout=Path(sys.argv[sys.argv.index("-o")+1])\n'+body+'\n');fake.chmod(0o755)
        work=self.root/'cases';work.mkdir()
        runner=module.Runner(fake,work,timeout)
        runner.case('case','mod test;',pattern='required rejection' if negative else None,execute=True,stale=stale)
        return runner.results[0]['issues']
    def reject_text(self,extra=''):
        return 'sys.stderr.write("error: required rejection\\n --> input.sts:1:1\\n'+extra+'")\nsys.exit(1)'
    def test_normal_rejection(self):self.assertEqual(self.run_fake(self.reject_text()),[])
    def test_accepted_negative_never_executes(self):
        sentinel=self.root/'executed'
        body=f'out.write_text("#!/usr/bin/env python3\\nfrom pathlib import Path\\nPath({str(sentinel)!r}).touch()\\n");out.chmod(0o755)'
        self.assertTrue(self.run_fake(body));self.assertFalse(sentinel.exists())
    def test_stale_artifact_fails(self):self.assertTrue(self.run_fake(self.reject_text(),stale=True))
    def test_symlink_negative_fails(self):self.assertTrue(self.run_fake('out.symlink_to("missing")\n'+self.reject_text()))
    def test_symlink_positive_fails(self):self.assertTrue(self.run_fake('out.symlink_to("/bin/true")',negative=False))
    def test_signal_fails(self):self.assertTrue(self.run_fake('os.kill(os.getpid(),15)'))
    def test_timeout_fails(self):self.assertTrue(self.run_fake('time.sleep(1)',timeout=.05))
    def test_internal_diagnostic_fails(self):self.assertTrue(self.run_fake(self.reject_text('internal compiler error\\n')))
    def test_warning_negative_fails(self):self.assertTrue(self.run_fake(self.reject_text('warning: wrong rejection\\n')))
    def test_warning_positive_fails(self):
        self.assertTrue(self.run_fake('out.write_text("unused");sys.stderr.write("warning: unexpected\\n")',negative=False))
    def test_diagnostic_mismatch_fails(self):self.assertTrue(self.run_fake('sys.stderr.write("error: something else\\n --> input.sts:1:1\\n");sys.exit(1)'))
    def test_missing_span_fails(self):self.assertTrue(self.run_fake('sys.stderr.write("error: required rejection\\n");sys.exit(1)'))
    def test_positive_unexpected_stdout_fails(self):self.assertTrue(self.run_fake('out.write_text("unused");print("unexpected")',negative=False))
    def test_positive_runtime_stderr_fails(self):
        self.assertTrue(self.run_fake('out.write_text("#!/bin/sh\\necho unexpected >&2\\n");out.chmod(0o755)',negative=False))
    def test_leak_warning_fails(self):self.assertTrue(self.run_fake(self.reject_text('(!!) [2026-10-10 00:00:00] Memory leak detected: 1 bytes at 0x1234\\n')))
    def test_negative_stdout_fails(self):self.assertTrue(self.run_fake('print("unexpected")\n'+self.reject_text()))
    def test_warning_summary_fails(self):self.assertTrue(self.run_fake(self.reject_text('1 warnings emitted\\n')))
    def test_linker_stage_fails(self):self.assertTrue(self.run_fake(self.reject_text('[2026-10-10 00:00:00] linking\\n')))
    def test_missing_inventory_fails(self):self.assertTrue(module.inventory_issues(['one'],['one','two']))
    def test_empty_inventory_fails(self):self.assertTrue(module.inventory_issues([],[]))
    def test_duplicate_inventory_fails(self):self.assertTrue(module.inventory_issues(['one','one'],['one']))
    def test_extra_inventory_fails(self):self.assertTrue(module.inventory_issues(['one','two'],['one']))
    def test_exact_inventory_passes(self):self.assertEqual(module.inventory_issues(['one','two'],['two','one']),[])
    def test_archive_rejection_before_import_codegen(self):
        result={'category':'exit','returncode':1,'stdout':'','stderr':
                'error: incompatible ABI metadata\n --> caller.sts:1:1\nrequired ABI 7 for target x86_64-unknown-linux-gnu\n'}
        self.assertEqual(archive_rejection_issues(result,self.root/'absent'),[])
        for stage in ['importing x','generating code','linking','archiving']:
            changed={**result,'stderr':result['stderr']+'[2026-10-10 00:00:00] '+stage+'\n'}
            self.assertTrue(archive_rejection_issues(changed,self.root/'absent'),stage)
if __name__=='__main__':unittest.main(verbosity=2)
