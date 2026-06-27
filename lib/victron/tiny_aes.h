/*
 * AES-128 (CTR mode only) — derived from the public-domain kokke/tiny-AES-c.
 * https://github.com/kokke/tiny-AES-c  (The Unlicense)
 *
 * Only the forward cipher + CTR are kept: Victron advertisement decryption uses
 * AES-128-CTR, where decryption is the same operation as encryption.
 */
#ifndef VICTRON_TINY_AES_H_
#define VICTRON_TINY_AES_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES_BLOCKLEN 16
#define AES_KEYLEN 16
#define AES_keyExpSize 176

struct AES_ctx {
    uint8_t RoundKey[AES_keyExpSize];
    uint8_t Iv[AES_BLOCKLEN];
};

void AES_init_ctx(struct AES_ctx* ctx, const uint8_t* key);
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);

/* Encrypts/decrypts `length` bytes of `buf` in place (CTR is symmetric).
   NOTE: uses a big-endian counter — not used by the Victron path, which needs
   a little-endian counter (see VictronDecrypt). */
void AES_CTR_xcrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

/* Encrypts one 16-byte block in place (ECB). Used to build CTR keystream with
   a custom (little-endian) counter. */
void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif /* VICTRON_TINY_AES_H_ */
