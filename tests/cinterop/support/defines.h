#ifndef CINTEROP_DEFINES_H
#define CINTEROP_DEFINES_H

/* `#define NAME <int>` constants — surfaced as Stasha int constants
 * via cheader's ch_extract_define preprocessor-line scanner.  Function-
 * like macros (`#define FOO(x) ...`) and string macros are deliberately
 * skipped — the runner test below does not reference them. */

#define MAX_BUF       4096
#define MIN_BUF       16
#define MAGIC_HEX     0xCAFE
#define NEG_ONE       -1
#define HEX_U_SUFFIX  0x1FU
#define LONG_SUFFIX   100000L
#define ULL_SUFFIX    0xFFULL

/* Ignored — function-like macro should NOT be surfaced as a constant. */
#define UNUSED_MACRO(x) ((x) + 1)

/* Ignored — non-integer body, parser must skip cleanly. */
#define GREETING "hello"

#endif
