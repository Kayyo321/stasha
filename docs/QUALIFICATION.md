# Qualification boundaries

The `Linux compiler qualification` workflow runs on main updates and pull
requests using Ubuntu 24.04 x86-64. It builds the repository's exact LLVM/LLD
submodule, rejects mismatched or modified LLVM source, and records toolchain and
compiler identity. Ubuntu's packaged C/C++ compiler is a bootstrap and independent
C oracle, not a substitute LLVM code-generation library.

The workflow runs strict diagnostic, signal, C interoperability, scalar, layout,
aggregate/callback ABI, member-address, cleanup, driver, and global-initializer
regressions. A failed stage prevents a PASS receipt. The command is
`bash scripts/qualify_linux.sh BIN EVIDENCE_DIRECTORY`; LLVM_BUILD selects the
pinned source-built toolchain. Build the C interoperability support archive first
with `make tests/cinterop/build/libcinterop_support.a`.

This is bounded hosted compiler qualification. Full standard-library, sanitizer,
K0/QEMU, hardware, interrupts, SMP, and broader memory/provenance qualification
have separate evidence and acceptance requirements. A green workflow does not
waive those requirements or make the development compiler release-qualified.

Native macOS, Windows, and Linux AArch64 release qualification is pending.
The release matrix fails these entries explicitly before packaging. Existing
platform implementation is retained, including the Windows bootstrap recipe,
but its unpinned prebuilt LLVM path cannot produce an approved release. The
combined release job remains blocked until every required native gate is
qualified. No release is automatically dispatched by the qualification workflow.

Runner labels were checked against GitHub's official
[runner image inventory](https://github.com/actions/runner-images).
The retired macOS 13 Intel entry uses `macos-15-intel`; see the official
[retirement announcement](https://github.blog/changelog/2025-09-19-github-actions-macos-13-runner-image-is-closing-down/).
Actions are pinned to full commit IDs. Workflow permissions are read-only except
for the pre-existing tag publication job's narrowly scoped release permission.
