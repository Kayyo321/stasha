#pragma once
#include <stdint.h>
#include <stddef.h>

/* Portable SHA-256 runtime shim (public-domain implementation).
   Replaces the macOS-only CommonCrypto CC_SHA256* API so the crypto
   stdlib module links identically on macOS, Linux and Windows. */

#define STS_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} sts_sha256_ctx;

void sts_sha256_init(sts_sha256_ctx *ctx);
void sts_sha256_update(sts_sha256_ctx *ctx, const uint8_t *data, uint64_t len);
void sts_sha256_final(uint8_t *digest, sts_sha256_ctx *ctx);

/* One-shot: digest must be >= 32 bytes. */
void sts_sha256(const uint8_t *data, uint64_t len, uint8_t *digest);
