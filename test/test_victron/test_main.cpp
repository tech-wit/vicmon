// Host unit tests for lib/victron — run with: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "BitReader.h"
#include "VictronDecrypt.h"
#include "VictronParser.h"
#include "tiny_aes.h"

// LSB-first bit writer mirroring victron::BitReader, used to build test records.
struct BitWriter {
    uint8_t* buf;
    size_t bitPos = 0;
    explicit BitWriter(uint8_t* b) : buf(b) {}
    void write(uint32_t value, int nbits) {
        for (int i = 0; i < nbits; ++i) {
            if ((value >> i) & 1u) buf[bitPos >> 3] |= (1u << (bitPos & 7));
            ++bitPos;
        }
    }
};

// NIST SP800-38A F.5.1 — AES-128 CTR known-answer test (first block).
void test_aes_ctr_nist_vector(void) {
    uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                       0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    uint8_t iv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                      0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
    uint8_t buf[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                       0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    uint8_t expected[16] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
                            0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    AES_CTR_xcrypt_buffer(&ctx, buf, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, 16);
}

static const uint8_t kKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// End-to-end: pack a known battery record, encrypt it, wrap with the Victron
// header, then decrypt+parse and confirm the values round-trip.
void test_battery_monitor_roundtrip(void) {
    uint8_t plain[16] = {0};
    BitWriter w(plain);
    w.write(600, 16);                                // ttg = 600 min
    w.write(static_cast<uint32_t>(1305) & 0xFFFF, 16);  // voltage 13.05 V
    w.write(0, 16);                                  // alarm
    w.write(static_cast<uint32_t>(1240) & 0xFFFF, 16);  // aux starter 12.40 V
    w.write(0, 2);                                   // aux mode = starter
    w.write(static_cast<uint32_t>(-5234) & 0x3FFFFF, 22);  // current -5.234 A
    w.write(123, 20);                                // consumed raw -> -12.3 Ah
    w.write(875, 10);                                // soc 87.5 %

    // Encrypt with little-endian CTR (one block): cipher = plain XOR keystream0.
    uint16_t nonce = 0x1234;
    uint8_t ks[16] = {0};
    ks[0] = nonce & 0xFF; ks[1] = (nonce >> 8) & 0xFF;
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, kKey);
    AES_ECB_encrypt(&ctx, ks);
    uint8_t cipher[16];
    for (int i = 0; i < 16; ++i) cipher[i] = plain[i] ^ ks[i];

    // Build extra manufacturer data: 8-byte header + ciphertext.
    uint8_t extra[8 + 16];
    extra[0] = 0x10; extra[1] = 0x00;             // 2-byte prefix
    extra[2] = 0xA3; extra[3] = 0x02;             // model id (arbitrary)
    extra[4] = 0x00;                              // readout type
    extra[5] = nonce & 0xFF; extra[6] = (nonce >> 8) & 0xFF;
    extra[7] = kKey[0];                           // key-check byte
    std::memcpy(extra + 8, cipher, 16);

    uint8_t out[32];
    int n = victron::decrypt(extra, sizeof(extra), kKey, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(16, n);

    victron::BatteryData b;
    TEST_ASSERT_TRUE(victron::parseBatteryMonitor(out, n, b));
    TEST_ASSERT_TRUE(b.ttgValid);
    TEST_ASSERT_EQUAL_UINT16(600, b.timeToGoMin);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.05f, b.voltage);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -5.234f, b.current);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 87.5f, b.soc);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -12.3f, b.consumedAh);
    TEST_ASSERT_EQUAL(victron::AuxMode::StarterVoltage, b.auxMode);
    TEST_ASSERT_TRUE(b.auxValid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.40f, b.auxValue);
}

// A wrong key (first byte mismatch) must be rejected, not silently decoded.
void test_decrypt_rejects_wrong_key(void) {
    // key-check byte at offset 7; 0xAA != key[0]=0x5b -> reject.
    uint8_t extra[16] = {0x10, 0, 0, 0, 0, 0x34, 0x12, 0xAA, 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t key[16] = {0x5b};
    uint8_t out[32];
    TEST_ASSERT_EQUAL_INT(-1, victron::decrypt(extra, sizeof(extra), key, out, sizeof(out)));
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_aes_ctr_nist_vector);
    RUN_TEST(test_battery_monitor_roundtrip);
    RUN_TEST(test_decrypt_rejects_wrong_key);
    UNITY_END();
}

int main(int, char**) {
    setup();
    return 0;
}
