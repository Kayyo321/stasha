#ifndef NEG_LONG_DOUBLE_H
#define NEG_LONG_DOUBLE_H

/* `long double` has a platform-specific ABI that Stasha does not model
 * (80-bit on x86, 128-bit on aarch64-darwin, 64-bit on aarch64-linux).
 * Silently mapping it to f64 would corrupt return values and arg slots,
 * so cheader must report it as unsupported. */

long double ld_identity(long double x);

#endif
