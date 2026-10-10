/* Strong nonreturning hosted oracle. Only reviewed canonical-empty fixtures
 * link this hook; no fabricated object/table/slot is executed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int32_t side_effects;
_Noreturn void __stasha_panic(uint32_t reason) {
    fprintf(stderr,"descriptor-panic reason=%u side-effects=%d\n",reason,side_effects);
    _Exit(reason == 7 && side_effects == 0 ? 77 : 78);
}
