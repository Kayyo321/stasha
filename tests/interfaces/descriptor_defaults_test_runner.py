#!/usr/bin/env python3
"""Mutation checks for compile gate; no compiler or generated fixture execution."""
import tempfile,unittest
from pathlib import Path
from descriptor_defaults_precheck import compile_matches
from descriptor_defaults_empty_run import validate_gate,EXPECTED
from descriptor_defaults_empty_cases import CASES
class GateTests(unittest.TestCase):
 def test_fail_closed(self):
  with tempfile.TemporaryDirectory() as t:
   d=Path(t);source=d/'case.sts';artifact=d/'out.ll';source.write_text('source');artifact.write_text('valid IR')
   ok={'returncode':0,'stdout':'','diagnostics':[],'artifact':True}
   self.assertTrue(compile_matches(ok,0,'','',artifact,source))
   for rc in [-11,'timeout',1]:self.assertFalse(compile_matches(ok,rc,'','',artifact,source))
   self.assertFalse(compile_matches(ok,0,'surprise','',artifact,source))
   self.assertFalse(compile_matches(ok,0,'','warning: surprise\n',artifact,source))
   artifact.write_text('stale artifact sentinel');self.assertFalse(compile_matches(ok,0,'','',artifact,source));artifact.unlink()
   self.assertFalse(compile_matches(ok,0,'','',artifact,source))
   reject={'returncode':1,'stdout':'','diagnostics':[['error','nil cannot initialize an interface descriptor']],'artifact':False}
   error=f'error: nil cannot initialize an interface descriptor\n --> {source}:1\n'
   self.assertTrue(compile_matches(reject,1,'',error,artifact,source))
   self.assertFalse(compile_matches(reject,0,'',error,artifact,source))
   self.assertFalse(compile_matches(reject,1,'',error.replace(str(source),'other.sts'),artifact,source))
   self.assertFalse(compile_matches(reject,1,'',error+'warning: surprise\n',artifact,source))
   artifact.write_text('stale');self.assertFalse(compile_matches(reject,1,'',error,artifact,source))
class EmptyGateTests(unittest.TestCase):
 def test_inventory_gate(self):
  gate={'compiler_sha256':'compiler','hook_sha256':'hook','allowed_optimizations':[0,1,2,3],'expected_runtime':EXPECTED,'source_fixture_count':len(CASES),'reviewed_ir':[{'name':name,'optimization':opt,'source_sha256':'source'} for name in CASES for opt in (0,3)]}
  self.assertEqual(set(validate_gate(gate,'compiler','hook')),set(CASES))
  for key,value in [('compiler_sha256','other'),('hook_sha256','other'),('allowed_optimizations',[0]),('expected_runtime',{}),('source_fixture_count',0)]:
   with self.assertRaises(RuntimeError):validate_gate({**gate,key:value},'compiler','hook')
  with self.assertRaises(RuntimeError):validate_gate({**gate,'reviewed_ir':gate['reviewed_ir'][:-1]},'compiler','hook')
  with self.assertRaises(RuntimeError):validate_gate({**gate,'reviewed_ir':gate['reviewed_ir']+gate['reviewed_ir'][:1]},'compiler','hook')
if __name__=='__main__':unittest.main()
