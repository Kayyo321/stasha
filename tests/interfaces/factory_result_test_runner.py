#!/usr/bin/env python3
"""Fail-closed source/category/oracle sentinels; all processes are mocked."""
import contextlib
from dataclasses import replace
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import factory_result_run as suite
from factory_result_cases import matrix, inventory

class Guards(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.compiler=self.root/'compiler';self.compiler.write_text('inert compiler bytes');self.calls=[]
        self.valid=next(c for c in matrix() if c.category=='valid')
        self.reject=next(c for c in matrix() if c.category=='reject')
        self.deferred=next(c for c in matrix() if c.category=='deferred')
    def run_cases(self,cases,*,phase='gate-b',frozen=None,scenario='accepted'):
        frozen=inventory(cases) if frozen is None else frozen;frozen=json.loads(json.dumps(frozen))
        def process(runner,command,stem):
            self.calls.append(list(map(str,command)));compile_step=str(command[0])==str(self.compiler)
            code=0;stderr='';category='exit';stdout=''
            if compile_step:
                out=Path(command[command.index('-o')+1]);case=cases[0]
                if scenario=='accepted':out.write_text('inert artifact')
                elif scenario=='changed-compiler':out.write_text('inert artifact');self.compiler.write_text('changed')
                else:
                    diagnostic=case.diagnostic_a if phase=='gate-a' else case.diagnostic_b
                    line,col=case.span_a if phase=='gate-a' else case.span_b;file=str(stem.parent/case.diagnostic_file)
                    if scenario=='wrong-span':col+=1
                    if scenario=='wrong-file':file=str(stem.parent/'other.sts')
                    if scenario=='same-basename-wrong-directory':file='/not-the-owned-source/'+Path(case.diagnostic_file).name
                    if scenario=='wrong-diagnostic':diagnostic='different failure'
                    stderr=f'error: {diagnostic}\n  --> {file}:{line}:{col}\n';code=1
                    if out.exists():out.unlink()
                    if scenario=='stale-artifact':out.write_text('stale')
                    if scenario=='signal':category='signal';code=-11
                    if scenario=='warning':stderr+='warning: wrong path\n'
            result=dict(command=list(map(str,command)),category=category,returncode=code,stdout=stdout,stderr=stderr)
            stem.with_suffix('.json').write_text(json.dumps(result));stem.with_suffix('.stdout').write_text(stdout);stem.with_suffix('.stderr').write_text(stderr)
            return result
        with patch.object(suite,'matrix',return_value=cases),patch.object(suite,'load_inventory',return_value=frozen),patch.object(suite,'LEVELS',(0,)),patch.object(suite.Runner,'process',process),patch.object(suite,'native_issues',return_value=[]),contextlib.redirect_stdout(io.StringIO()):
            failed=suite.run(self.compiler,self.root/'evidence',phase)
        summary=json.loads(next((self.root/'evidence').glob('*/summary.json')).read_text());return failed,summary
    def no_process(self,cases,**kw):
        failed,summary=self.run_cases(cases,**kw);self.assertTrue(failed);self.assertTrue(summary['inventory_issues']);self.assertEqual([],self.calls);self.assertEqual([],summary['results'])
    def test_source_drift(self):self.no_process([replace(self.valid,source=self.valid.source+'\n// drift')],frozen=inventory([self.valid]))
    def test_reject_reclassification(self):self.no_process([replace(self.reject,category='valid',diagnostic_b=None,span_b=None,expected_exit=0,expected_stdout='',expected_stderr='')],frozen=inventory([self.reject]))
    def test_deferred_reclassification(self):self.no_process([replace(self.deferred,category='valid',expected_exit=0,expected_stdout='',expected_stderr='')],frozen=inventory([self.deferred]))
    def test_unknown_category(self):self.no_process([replace(self.deferred,category='unknown')])
    def test_missing_runtime(self):self.no_process([replace(self.valid,expected_exit=None)])
    def test_bool_runtime_is_not_integer(self):self.no_process([replace(self.valid,expected_exit=False)])
    def test_reject_runtime(self):self.no_process([replace(self.reject,expected_exit=0)])
    def test_deferred_runtime(self):self.no_process([replace(self.deferred,expected_stdout='')])
    def test_missing_span(self):self.no_process([replace(self.reject,span_b=None)])
    def test_missing_gate_a_span(self):self.no_process([replace(self.valid,span_a=None)])
    def test_deferred_diagnostic(self):self.no_process([replace(self.deferred,diagnostic_b='bad')])
    def test_support_overwrite(self):self.no_process([replace(self.valid,files={'input.sts':'changed'})])
    def test_support_escape(self):self.no_process([replace(self.valid,files={'../escape.sts':'changed'})])
    def test_wrong_diagnostic_owner(self):self.no_process([replace(self.reject,diagnostic_file='outside.sts')])
    def test_empty_inventory(self):self.no_process([])
    def test_duplicate_inventory(self):self.no_process([self.valid],frozen=inventory([self.valid,self.valid]))
    def test_wrong_phase(self):self.no_process([self.valid],phase='other')
    def test_negative_acceptance_never_runs(self):
        failed,_=self.run_cases([self.reject]);self.assertTrue(failed);self.assertEqual(len(self.calls),1);self.assertEqual(self.calls[0][1],'ir')
    def test_gate_a_positive_never_runs(self):
        failed,_=self.run_cases([self.valid],phase='gate-a');self.assertTrue(failed);self.assertEqual(len(self.calls),1);self.assertEqual(self.calls[0][1],'ir')
    def test_gate_b_valid_runs(self):
        failed,_=self.run_cases([self.valid]);self.assertFalse(failed);self.assertEqual(len(self.calls),3)
    def test_deferred_never_runs(self):
        failed,_=self.run_cases([self.deferred]);self.assertFalse(failed);self.assertEqual(len(self.calls),1);self.assertEqual(self.calls[0][1],'ir')
    def test_exact_negative(self):
        failed,_=self.run_cases([self.reject],scenario='rejected');self.assertFalse(failed);self.assertEqual(len(self.calls),1)
    def test_negative_malformed_evidence(self):
        for scenario in ['wrong-span','wrong-file','same-basename-wrong-directory','wrong-diagnostic','stale-artifact','signal','warning']:
            with self.subTest(scenario=scenario):
                self.calls.clear();failed,_=self.run_cases([self.reject],scenario=scenario);self.assertTrue(failed);self.assertEqual(len(self.calls),1)
    def test_compiler_change(self):
        failed,summary=self.run_cases([self.deferred],scenario='changed-compiler');self.assertTrue(failed);self.assertIn('compiler changed during qualification',summary['inventory_issues'])

class NativeGuards(unittest.TestCase):
    def setUp(self):self.case=next(c for c in matrix() if c.name=='holder-copy-17')
    def text(self):return "define internal %H @make(i32 %0) {\n  %raw = load %H, ptr %h\n  ret %H %raw\n}\ndefine i32 @main() {\n  %v = call %H @make(i32 17)\n  store %H %v, ptr %out\n  ret i32 0\n}\n"
    def test_native_valid(self):self.assertEqual(suite.native_issues(self.case,self.text(),0),[])
    def test_return_replacement(self):self.assertTrue(suite.native_issues(self.case,self.text().replace('ret %H %raw','ret %H zeroinitializer'),0))
    def test_extra_return(self):self.assertTrue(suite.native_issues(self.case,self.text().replace('ret %H %raw','ret %H %raw\n  ret %H zeroinitializer'),0))
    def test_callee_drift(self):self.assertTrue(suite.native_issues(self.case,self.text().replace('call %H @make','call %H @other'),0))
    def test_copyout_missing(self):self.assertTrue(suite.native_issues(self.case,self.text().replace('store %H %v, ptr %out',''),0))
    def test_public_native(self):self.assertTrue(suite.native_issues(self.case,self.text().replace('define internal','define'),0))

if __name__=='__main__':unittest.main()
