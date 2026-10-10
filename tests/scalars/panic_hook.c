/* Test-only strong override of the common Stasha panic ABI.
 * Link this OBJECT directly with `stasha ... -l /absolute/panic_hook.o`.
 * An ordinary archive can remain unextracted when a weak default already
 * satisfies the panic symbol. Never include this hook in production binaries.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void __stasha_panic(uint32_t reason) {
    const char *name;
    switch (reason) {
        case 1: name = "overflow"; break;
        case 2: name = "division-by-zero"; break;
        case 3: name = "division-overflow"; break;
        case 4: name = "invalid-shift"; break;
        case 5: name = "fp-cast-range"; break;
        /* ABI values 6/7/8 are reserved for bounds/null/allocation. */
        default:
            fprintf(stderr, "STASHA_TRAP:unknown:%u\n", (unsigned)reason);
            fflush(stderr);
            _Exit(87);
    }
    fprintf(stderr, "STASHA_TRAP:%s\n", name);
    fflush(stderr);
    _Exit(86);
}
