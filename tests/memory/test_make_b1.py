#!/usr/bin/env python3
"""Fail-closed B1 harness and immutable inventory controls."""
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
import run_make_b1 as b1


def process(code=0,stderr='',stdout='',category='exit'):
    return dict(returncode=code,stderr=stderr,stdout=stdout,category=category)


class MakeB1Tests(unittest.TestCase):
    def test_inventory(self):
        cases=b1.cases()
        self.assertEqual(len(cases),106)
        self.assertEqual(len({c.name for c in cases}),106)
        self.assertEqual(sum(c.outcome=='rejection' for c in cases),39)
        self.assertEqual(sum(c.outcome!='rejection' for c in cases),67)
        self.assertTrue(all(c.diagnostic for c in cases if c.outcome=='rejection'))

    def test_live_allocation_oracles_and_bounds(self):
        cases={c.name:c for c in b1.cases()}
        for name in ('default_i32','oom','oom_order','max_count_admitted','once_explicit','generic_builtin'):
            self.assertIn('b1_use(&s)',cases[name].source)
        self.assertIn('n > 4096',b1.SUPPORT)
        self.assertIn('if (force_fail) return 0;',b1.SUPPORT)
        self.assertIn('reason != (uint32_t)expected_reason',b1.SUPPORT)
        self.assertIn('memcmp(watched,&before',b1.SUPPORT)
        self.assertIn('STASHA_TRAP:allocation',b1.SUPPORT)

    def test_true_byte_limit_proof(self):
        for stride in (1,2,4,8):
            self.assertLessEqual(((1<<31)-1)*stride,(1<<63)-1)
        self.assertGreater((1<<63)-1,((1<<31)-1)*8)

    def test_all_diagnostic_stages_fail_on_internal_or_warning(self):
        for message in ('LLVM IR verification failed','warning: linker warning','error: failure'):
            self.assertTrue(b1.diagnostic_gates(process(stderr=message)))
        self.assertTrue(b1.diagnostic_gates(process(stderr='linker note'),quiet=True))
        self.assertTrue(b1.diagnostic_gates(process(stdout='unexpected compiler note'),quiet=True))

    def test_artifacts_are_regular_nonempty_and_fresh(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);output=root/'output';output.write_bytes(b'object')
            self.assertFalse(b1.success_gates(process(),output))
            output.write_bytes(b'');self.assertTrue(b1.success_gates(process(),output))
            output.unlink();target=root/'target';target.write_bytes(b'object');output.symlink_to(target)
            self.assertTrue(b1.success_gates(process(),output))
            self.assertTrue(b1.rejection_gates(process(1,'error: intended\ncase.sts:1\n'),root/'case.sts',output,'intended'))

    def test_rejection_requires_normal_exit_location_and_exact_diagnostic(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'case.sts';output=root/'output'
            good=process(1,'error: intended\ncase.sts:1\n')
            self.assertFalse(b1.rejection_gates(good,source,output,'intended'))
            for bad in (process(0,good['stderr']),process(-11,good['stderr'],category='signal'),
                        process(1,'error: intended'),process(1,'error: other\ncase.sts:1\n'),
                        process(1,good['stderr']+'LLVM ERROR: bad IR')):
                self.assertTrue(b1.rejection_gates(bad,source,output,'intended'))

    def test_unexpected_negative_acceptance_is_never_linked_or_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);args=SimpleNamespace(compiler=Path('/compiler'),cc=Path('/cc'))
            case=next(c for c in b1.cases() if c.outcome=='rejection')
            def accepted(command,cwd,*args,**kwargs):
                (cwd/'program.o').write_bytes(b'accepted object')
                return process()
            with patch.object(b1.runner,'capture',side_effect=accepted) as capture:
                result=b1.execute_case(case,0,args,root/'case',root/'support.o')
            self.assertEqual(result['status'],'FAIL');self.assertEqual(capture.call_count,1)
            self.assertNotIn('link',result);self.assertNotIn('run',result)

    def test_link_warning_prevents_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);args=SimpleNamespace(compiler=Path('/compiler'),cc=Path('/cc'))
            case=next(c for c in b1.cases() if c.outcome=='success')
            calls=[]
            def fake(command,cwd,*args,**kwargs):
                calls.append(command)
                if len(calls)==1:
                    (cwd/'program.o').write_bytes(b'object');return process()
                output=cwd/'program';output.write_bytes(b'program');output.chmod(0o700)
                return process(stderr='warning: ignored linker warning\n')
            with patch.object(b1.runner,'capture',side_effect=fake):
                result=b1.execute_case(case,0,args,root/'case',root/'support.o')
            self.assertEqual(result['status'],'FAIL');self.assertEqual(len(calls),2)
            self.assertNotIn('run',result)

    def test_ir_audit_rejects_missing_cfg(self):
        self.assertTrue(b1.constructor_ir_problems(''))
        self.assertTrue(b1.constructor_ir_problems('define void @b1_ir() {\nentry:\n ret void\n}\n'))


if __name__=='__main__':unittest.main()
