#pragma once
#include <stdint.h>
#include <stddef.h>

/* Portable regex runtime for the stsstdlib/regex module.

   A self-contained recursive-backtracking engine — no POSIX <regex.h>,
   no PCRE — so the same code path runs on macOS, Linux, and Windows.

   Supported syntax (subset of POSIX ERE; sufficient for the stdlib
   test surface):
     literal characters         x
     any char                   .
     anchors                    ^ $
     escape                     \\ \. \* \+ \? \[ \] \( \) \d \D \w \W \s \S
     character class            [abc]   [^abc]   [a-z0-9]
     quantifiers                * + ?       (greedy)
     concatenation              ab
   No grouping, alternation, backrefs.

   The "extended" flag is accepted for source compatibility with the
   previous POSIX-backed module but is ignored — the same engine handles
   every pattern.

   ── API ──
   sts_regex_compile  — compiles `pattern` (NUL-terminated) into the
                        opaque ctx buffer.  Caller must pass a buffer
                        of at least sts_regex_ctx_size bytes; the buffer
                        is owned by the caller (heap or stack) and may
                        be freed without any teardown call once you're
                        done matching with it.
                        Returns 0 on success, non-zero on syntax error.

   sts_regex_test     — returns 1 if `text` contains a match, else 0.

   sts_regex_exec     — like _test but writes the match's [start, end)
                        byte offsets into *out_start, *out_end when a
                        match is found.  Returns 1/0.
*/

#define STS_REGEX_FLAG_ICASE 1   /* not implemented; reserved */

extern const size_t sts_regex_ctx_size;

int sts_regex_compile(void *ctx, const char *pattern, int flags);
int sts_regex_test(const void *ctx, const char *text);
int sts_regex_exec(const void *ctx, const char *text,
                   int32_t *out_start, int32_t *out_end);
