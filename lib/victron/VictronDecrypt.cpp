#include "VictronDecrypt.h"

#include <cstring>

#include "tiny_aes.h"

namespace victron {

int decrypt(const uint8_t* extra, size_t extraLen, const uint8_t key[16],
            uint8_t* out, size_t outCap) {
    // Header is 8 bytes ([0..7]); ciphertext starts at [8].
    if (extra == nullptr || extraLen < 9) return -1;
    if (extra[0] != 0x10) return -1;       // not a product advertisement
    if (extra[7] != key[0]) return -1;     // key-check byte doesn't match

    size_t ctLen = extraLen - 8;
    if (ctLen > outCap) ctLen = outCap;
    std::memcpy(out, extra + 8, ctLen);

    // Victron uses AES-128-CTR with a LITTLE-ENDIAN counter starting at the
    // 16-bit IV. Build the keystream block-by-block so the counter increments
    // correctly regardless of payload length.
    uint16_t nonce = static_cast<uint16_t>(extra[5]) |
                     (static_cast<uint16_t>(extra[6]) << 8);

    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);

    uint8_t keystream[AES_BLOCKLEN];
    for (size_t off = 0; off < ctLen; off += AES_BLOCKLEN) {
        uint32_t counter = static_cast<uint32_t>(nonce) +
                           static_cast<uint32_t>(off / AES_BLOCKLEN);
        // 128-bit little-endian counter block (low byte first).
        std::memset(keystream, 0, AES_BLOCKLEN);
        keystream[0] = static_cast<uint8_t>(counter & 0xFF);
        keystream[1] = static_cast<uint8_t>((counter >> 8) & 0xFF);
        keystream[2] = static_cast<uint8_t>((counter >> 16) & 0xFF);
        keystream[3] = static_cast<uint8_t>((counter >> 24) & 0xFF);
        AES_ECB_encrypt(&ctx, keystream);

        size_t block = ctLen - off;
        if (block > AES_BLOCKLEN) block = AES_BLOCKLEN;
        for (size_t i = 0; i < block; ++i) out[off + i] ^= keystream[i];
    }

    return static_cast<int>(ctLen);
}

}  // namespace victron
