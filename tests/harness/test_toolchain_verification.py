"""Fail-closed unit tests for the source/build identity receipt (no LLVM needed)."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('verify_toolchain', ROOT/'scripts/verify_llvm_toolchain.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class ToolchainReceiptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.build = Path(self.tmp.name)
        (self.build/'bin').mkdir(); (self.build/'lib').mkdir()
        (self.build/'bin/llvm-config').write_bytes(b'unit-test-placeholder')
        for name in ('Common','ELF','MachO','COFF','MinGW','Wasm'):
            (self.build/'lib'/('liblld'+name+'.a')).write_bytes(b'archive')
        (self.build/'CMakeCache.txt').write_text('LLVM_SOURCE_DIR:STATIC=/fake/llvm\nLLVM_ENABLE_PROJECTS:STRING=clang;lld\n')
        self.actual = 'pinned'; self.dirty = ''; self.version = '23.0.0git'; self.targets = 'X86 AArch64'
    def run_tool(self, *args, **kwargs):
        words = tuple(map(str,args))
        if words == ('git','rev-parse','HEAD:extlib/llvm'): return 'pinned'
        if words == ('git','rev-parse','HEAD'): return self.actual
        if words[:2] == ('git','status'): return self.dirty
        return {'--version':self.version, '--targets-built':self.targets,
                '--assertion-mode':'OFF','--has-rtti':'YES','--host-target':'x86_64-unknown-linux-gnu'}[words[-1]]
    def invoke(self):
        with patch.object(verifier,'run',side_effect=self.run_tool), patch('sys.argv',['verify','--llvm-build',str(self.build)]), contextlib.redirect_stdout(io.StringIO()) as out:
            verifier.main()
        return out.getvalue()
    def test_valid(self): self.assertIn('"llvm_revision": "pinned"',self.invoke())
    def test_wrong_revision(self):
        self.actual='other'
        with self.assertRaisesRegex(ValueError,'differs from gitlink'): self.invoke()
    def test_dirty(self):
        self.dirty=' M llvm/lib/a.cpp'
        with self.assertRaisesRegex(ValueError,'tracked modifications'): self.invoke()
    def test_wrong_version(self):
        self.version='19.1.0'
        with self.assertRaisesRegex(ValueError,'LLVM23'): self.invoke()
    def test_missing_target(self):
        self.targets='AArch64'
        with self.assertRaisesRegex(ValueError,'X86'): self.invoke()
    def test_missing_lld_project(self):
        (self.build/'CMakeCache.txt').write_text('LLVM_SOURCE_DIR:STATIC=/fake/llvm\nLLVM_ENABLE_PROJECTS:STRING=clang\n')
        with self.assertRaisesRegex(ValueError,'include LLD'): self.invoke()
    def test_missing_archives(self):
        (self.build/'lib/liblldCommon.a').unlink()
        with self.assertRaisesRegex(ValueError,'libraries are missing'): self.invoke()
    def test_empty_archive(self):
        (self.build/'lib/liblldELF.a').write_bytes(b'')
        with self.assertRaisesRegex(ValueError,'missing or empty'): self.invoke()
    def test_failed_rerun_removes_previous_receipt(self):
        receipt=self.build/'receipt.json'; receipt.write_text('{\"old\": true}')
        self.actual='other'
        with patch.object(verifier,'run',side_effect=self.run_tool), patch('sys.argv',['verify','--llvm-build',str(self.build),'--output',str(receipt)]):
            with self.assertRaises(ValueError): verifier.main()
        self.assertFalse(receipt.exists())
    def test_cache_values_preserve_equals(self):
        p=self.build/'cache';p.write_text('// comment\n# comment\nA:STRING=a=b\nnot-a-value\n')
        self.assertEqual(verifier.cache_entries(p),{'A':'a=b'})

if __name__ == '__main__': unittest.main()
