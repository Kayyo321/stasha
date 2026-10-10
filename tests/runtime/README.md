# Zone allocator regression

Run with the host C compiler:

```
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
  tests/runtime/zone_alignment.c src/runtime/zone_runtime.c -o /tmp/zone-test
ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 /tmp/zone-test
```

Checks max_align_t alignment for sizes 0–256, block rollover, large allocation,
size arithmetic overflow, move-copy independence and repeated release. LSan is
disabled only for the traced audit environment; this is not a leak-sanitizer
qualification. Source objects with alignment above max_align_t are unsupported.
