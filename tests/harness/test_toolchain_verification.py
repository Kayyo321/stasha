"""Fail-closed unit tests for the source/build identity receipt (no LLVM needed)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
PINNED = '348061d58dfe5e59027a0d6d51f916197cdd8d68'
spec = importlib.util.spec_from_file_location('verify_toolchain', ROOT/'scripts/verify_llvm_toolchain.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

class ToolchainReceiptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root/'build'; self.build.mkdir()
        (self.build/'bin').mkdir(); (self.build/'lib').mkdir()
        (self.build/'bin/llvm-config').write_bytes(b'unit-test-placeholder')
        for name in ('Common','ELF','MachO','COFF','MinGW','Wasm'):
            (self.build/'lib'/('liblld'+name+'.a')).write_bytes(b'archive')
        self.write_cache('lld')
        self.actual = PINNED; self.dirty = ''; self.version = '23.0.0git'; self.targets = 'X86 AArch64'
        self.clang = self.build/'bin/clang-23'
        self.resource = self.build/'lib/clang/23'
        self.clang_version = f'clang version 23.0.0git (https://github.com/llvm/llvm-project.git {PINNED})\nTarget: x86_64-unknown-linux-gnu\nThread model: posix'
        self.calls = []

    def write_cache(self, projects):
        (self.build/'CMakeCache.txt').write_text(f'LLVM_SOURCE_DIR:STATIC=/fake/llvm\nLLVM_ENABLE_PROJECTS:STRING={projects}\n')

    def add_clang(self):
        self.write_cache('lld;clang')
        self.clang.write_bytes(b'unit-test-clang-placeholder'); self.clang.chmod(0o755)
        (self.build/'bin/clang').symlink_to(self.clang.name)
        (self.resource/'include').mkdir(parents=True)
        for name in ('stdint.h', 'stddef.h', 'stdarg.h'):
            (self.resource/'include'/name).write_text('/* unit-test header */\n')

    def run_tool(self, *args, **kwargs):
        words = tuple(map(str,args)); self.calls.append(words)
        if words == ('git','rev-parse','HEAD:extlib/llvm'): return PINNED
        if words == ('git','rev-parse','HEAD'): return self.actual
        if words[:2] == ('git','status'): return self.dirty
        if words[0] == str(self.clang):
            return {'--version':self.clang_version, '--print-resource-dir':str(self.resource)}[words[-1]]
        if words[0] != str(self.build/'bin/llvm-config'):
            raise AssertionError(f'unexpected tool: {words}')
        return {'--version':self.version, '--targets-built':self.targets,
                '--assertion-mode':'OFF','--has-rtti':'YES','--host-target':'x86_64-unknown-linux-gnu'}[words[-1]]

    def invoke(self, *options):
        with patch.object(verifier,'run',side_effect=self.run_tool), patch('sys.argv',['verify','--llvm-build',str(self.build),*map(str,options)]), contextlib.redirect_stdout(io.StringIO()) as out:
            verifier.main()
        return json.loads(out.getvalue())

    def test_valid_llvm_lld_only_default(self):
        receipt = self.invoke()
        self.assertEqual(receipt['llvm_revision'],PINNED)
        self.assertNotIn('clang',receipt)
        self.assertFalse(any('clang' in call[0] for call in self.calls))

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
        self.write_cache('clang')
        with self.assertRaisesRegex(ValueError,'include LLD'): self.invoke()
    def test_missing_archives(self):
        (self.build/'lib/liblldCommon.a').unlink()
        with self.assertRaisesRegex(ValueError,'libraries are missing'): self.invoke()
    def test_empty_archive(self):
        (self.build/'lib/liblldELF.a').write_bytes(b'')
        with self.assertRaisesRegex(ValueError,'missing or empty'): self.invoke()
    def test_failed_rerun_removes_previous_receipt(self):
        receipt=self.root/'receipt.json'; receipt.write_text('{"old": true}')
        self.actual='other'
        with self.assertRaises(ValueError): self.invoke('--output',receipt)
        self.assertFalse(receipt.exists())
    def test_cache_values_preserve_equals(self):
        p=self.build/'cache';p.write_text('// comment\n# comment\nA:STRING=a=b\nnot-a-value\n')
        self.assertEqual(verifier.cache_entries(p),{'A':'a=b'})

    def test_valid_clang_receipt_resolves_symlink_and_hashes_resources(self):
        self.add_clang()
        output = self.root/'new/receipt.json'
        receipt = self.invoke('--require-clang','--output',output)
        self.assertEqual(json.loads(output.read_text()),receipt)
        self.assertEqual(receipt['clang']['path'],str(self.clang))
        self.assertEqual(receipt['clang']['sha256'],hashlib.sha256(self.clang.read_bytes()).hexdigest())
        self.assertEqual(receipt['clang']['version'],self.clang_version)
        self.assertEqual(receipt['clang']['target'],'x86_64-unknown-linux-gnu')
        self.assertEqual(receipt['clang']['resource_dir'],str(self.resource))
        for name, identity in receipt['clang']['resource_headers'].items():
            header = self.resource/'include'/name
            self.assertEqual(identity,{'path':str(header),'sha256':hashlib.sha256(header.read_bytes()).hexdigest()})

    def test_requires_clang_project(self):
        with self.assertRaisesRegex(ValueError,'include Clang'): self.invoke('--require-clang')
    def test_requires_clang_even_if_host_path_has_one(self):
        self.write_cache('lld;clang')
        host=self.root/'host-tools';host.mkdir()
        (host/'clang').write_text('#!/bin/sh\nexit 0\n');(host/'clang').chmod(0o755)
        with patch.dict(os.environ,{'PATH':str(host)}):
            with self.assertRaises(FileNotFoundError): self.invoke('--require-clang')
        self.assertFalse(any('clang' in call[0] for call in self.calls))
    def test_requires_nonempty_executable_clang(self):
        self.add_clang()
        for content, mode in ((b'',0o755),(b'not executable',0o644)):
            with self.subTest(content=content,mode=mode):
                self.clang.write_bytes(content);self.clang.chmod(mode)
                with self.assertRaisesRegex(ValueError,'empty, or not executable'): self.invoke('--require-clang')
    def test_rejects_clang_directory(self):
        self.add_clang(); self.clang.unlink(); self.clang.mkdir()
        with self.assertRaisesRegex(ValueError,'not executable'): self.invoke('--require-clang')
    def test_rejects_clang_symlink_outside_build(self):
        self.add_clang(); foreign = self.root/'foreign-clang';foreign.write_bytes(b'foreign');foreign.chmod(0o755)
        self.clang.unlink(); self.clang.symlink_to(foreign)
        with self.assertRaisesRegex(ValueError,'Clang executable resolves outside'): self.invoke('--require-clang')
    def test_rejects_wrong_clang_versions(self):
        self.add_clang()
        for version in ('19.1.0','23.0.0','23.0.0git-extra','23.1.0git'):
            with self.subTest(version=version):
                self.clang_version = f'clang version {version} ({PINNED})\nTarget: x86_64-unknown-linux-gnu'
                with self.assertRaisesRegex(ValueError,'Clang 23.0.0git'): self.invoke('--require-clang')
    def test_rejects_missing_wrong_short_or_extended_revision(self):
        self.add_clang()
        for revision in ('','0'*40,PINNED[:12],PINNED+'a','a'+PINNED):
            with self.subTest(revision=revision):
                self.clang_version = f'clang version 23.0.0git ({revision})\nTarget: x86_64-unknown-linux-gnu'
                with self.assertRaisesRegex(ValueError,'full pinned LLVM git revision'): self.invoke('--require-clang')
    def test_rejects_revision_only_on_another_line(self):
        self.add_clang();self.clang_version = f'clang version 23.0.0git\nTarget: x86_64-unknown-linux-gnu\n{PINNED}'
        with self.assertRaisesRegex(ValueError,'full pinned LLVM git revision'): self.invoke('--require-clang')
    def test_rejects_foreign_or_ambiguous_clang_targets(self):
        self.add_clang()
        first = self.clang_version.splitlines()[0]
        for target in ('aarch64-unknown-linux-gnu','x86_64-apple-darwin','x86_64-pc-windows-msvc','i686-pc-linux-gnu','x86_64-unknown-linux-gnux32','','x86_64-unknown-linux-gnu\nTarget: x86_64-pc-linux-gnu'):
            with self.subTest(target=target):
                self.clang_version = first+'\nTarget: '+target
                with self.assertRaisesRegex(ValueError,'Linux x86-64 GNU'): self.invoke('--require-clang')
    def test_accepts_compatible_linux_vendor_target(self):
        self.add_clang();self.clang_version=self.clang_version.replace('x86_64-unknown-linux-gnu','x86_64-pc-linux-gnu')
        self.assertEqual(self.invoke('--require-clang')['clang']['target'],'x86_64-pc-linux-gnu')
    def test_rejects_foreign_or_relative_resource_directory(self):
        self.add_clang()
        for resource in (self.root,Path('relative-resource-dir')):
            with self.subTest(resource=resource):
                self.resource = resource
                with self.assertRaisesRegex(ValueError,'outside selected|absolute path'): self.invoke('--require-clang')
    def test_rejects_resource_directory_symlink_outside_build(self):
        self.add_clang(); link=self.build/'foreign-resource';link.symlink_to(self.root, target_is_directory=True);self.resource=link
        with self.assertRaisesRegex(ValueError,'resource directory resolves outside'): self.invoke('--require-clang')
    def test_rejects_missing_resource_directory(self):
        self.add_clang();self.resource=self.build/'missing'
        with self.assertRaises(FileNotFoundError): self.invoke('--require-clang')
    def test_rejects_resource_file_instead_of_directory(self):
        self.add_clang();self.resource=self.clang
        with self.assertRaisesRegex(ValueError,'not a directory'): self.invoke('--require-clang')
    def test_rejects_missing_empty_or_foreign_required_headers(self):
        self.add_clang(); foreign=self.root/'foreign.h';foreign.write_text('/* foreign */')
        for name in ('stdint.h','stddef.h','stdarg.h'):
            header=self.resource/'include'/name
            for state in ('missing','empty','foreign'):
                with self.subTest(name=name,state=state):
                    header.unlink(missing_ok=True)
                    if state=='empty':header.touch()
                    if state=='foreign':header.symlink_to(foreign)
                    with self.assertRaises((ValueError,FileNotFoundError)): self.invoke('--require-clang')
            header.unlink();header.write_text('/* restored */')
    def test_clang_failure_invalidates_previous_receipt(self):
        self.add_clang(); output=self.root/'receipt.json'
        self.invoke('--require-clang','--output',output)
        self.clang_version=self.clang_version.replace(PINNED,'0'*40)
        with self.assertRaises(ValueError): self.invoke('--require-clang','--output',output)
        self.assertFalse(output.exists())
    def test_clang_command_failure_invalidates_previous_receipt(self):
        self.add_clang();output=self.root/'receipt.json'
        original=self.run_tool
        for option in ('--version','--print-resource-dir'):
            with self.subTest(option=option):
                output.write_text('{"stale":true}')
                def fail_clang(*args,**kwargs):
                    if str(args[0])==str(self.clang) and args[-1]==option:
                        raise subprocess.CalledProcessError(73,args)
                    return original(*args,**kwargs)
                with patch.object(self,'run_tool',side_effect=fail_clang):
                    with self.assertRaises(subprocess.CalledProcessError):self.invoke('--require-clang','--output',output)
                self.assertFalse(output.exists())
    def test_default_rerun_does_not_reuse_previous_clang_receipt(self):
        self.add_clang();output=self.root/'receipt.json'
        self.invoke('--require-clang','--output',output)
        self.write_cache('lld');self.clang.unlink()
        receipt=self.invoke('--output',output)
        self.assertNotIn('clang',receipt)
        self.assertEqual(json.loads(output.read_text()),receipt)

if __name__ == '__main__': unittest.main()
