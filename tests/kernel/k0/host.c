#include "component.h"
#include "run_config.h"
#include <stdio.h>
#include <stdlib.h>
_Noreturn void k0_panic_impl(uint32_t reason) {
    fprintf(stderr, "K0_PANIC run=%s build=%s reason=%u\n", K0_RUN_ID, K0_BUILD_ID, reason);
    fflush(stderr);
    _Exit(80 + (int)reason);
}
int main(int argc, char **argv) {
    if (argc == 2) k0_trigger_panic((unsigned)strtoul(argv[1], NULL, 10));
    uint32_t status = k0_reference_tests();
    printf("K0_COMPLETE run=%s build=%s status=%u observations=%llu checksum=%016llx\n",
        K0_RUN_ID, K0_BUILD_ID, status, (unsigned long long)k0_observations,
        (unsigned long long)k0_result_digest);
    return status ? 1 : 0;
}
