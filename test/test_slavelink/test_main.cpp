// Host unit tests for the master<->slave wire format (lib/slavelink/SlaveLink.h).
// Run with: pio test -e native
#include <string.h>
#include <unity.h>

#include "SlaveLink.h"

using namespace slavelink;

void setUp() {}
void tearDown() {}

void test_header() {
    Snapshot s = {};
    TEST_ASSERT_FALSE(validHeader(s));
    fillHeader(s);
    TEST_ASSERT_TRUE(validHeader(s));
    s.version = 99;  // a mismatched slave must reject, not misread
    TEST_ASSERT_FALSE(validHeader(s));
}

void test_roundtrip_bytes() {
    Snapshot s = {};
    fillHeader(s);
    s.mode = M_DISCHARGING;
    s.valid = V_SOC | V_BATTV | V_BATTA;
    s.soc_d = 565;
    s.battV_cv = 1314;
    s.battA_da = -123;
    s.masterId = 0xA1B2C3D4;
    s.flags = F_PAIRING;
    s.solarW_w = 240;
    s.dcdcInV_cv = 1385;
    s.consumedAh_da = -123;
    s.seq = 42;
    s.uptime_s = 12345;

    uint8_t buf[sizeof(Snapshot)];
    memcpy(buf, &s, sizeof(s));  // the exact bytes that go over ESP-NOW
    Snapshot r;
    memcpy(&r, buf, sizeof(r));

    TEST_ASSERT_TRUE(validHeader(r));
    TEST_ASSERT_EQUAL_UINT8(M_DISCHARGING, r.mode);
    TEST_ASSERT_EQUAL_UINT16(V_SOC | V_BATTV | V_BATTA, r.valid);
    TEST_ASSERT_EQUAL_INT16(565, r.soc_d);
    TEST_ASSERT_EQUAL_INT16(1314, r.battV_cv);
    TEST_ASSERT_EQUAL_INT16(-123, r.battA_da);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4, r.masterId);
    TEST_ASSERT_EQUAL_UINT8(F_PAIRING, r.flags);
    TEST_ASSERT_EQUAL_INT16(240, r.solarW_w);
    TEST_ASSERT_EQUAL_INT16(1385, r.dcdcInV_cv);
    TEST_ASSERT_EQUAL_INT16(-123, r.consumedAh_da);
    TEST_ASSERT_EQUAL_UINT16(42, r.seq);
    TEST_ASSERT_EQUAL_UINT32(12345, r.uptime_s);
}

void test_encode_decode() {
    TEST_ASSERT_EQUAL_INT16(565, encDeci(true, 56.5f));
    TEST_ASSERT_EQUAL_INT16(kNA, encDeci(false, 56.5f));
    TEST_ASSERT_EQUAL_INT16(1314, encCenti(true, 13.14f));
    TEST_ASSERT_EQUAL_INT16(-205, encDeci(true, -20.5f));  // negative rounds away from 0
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 56.5f, decDeci(565));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 13.14f, decCenti(1314));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, decDeci(kNA));  // n/a decodes to 0
}

void test_fits_espnow() {
    // Must stay well under the 250-byte ESP-NOW payload limit.
    TEST_ASSERT_LESS_THAN(250, (int)sizeof(Snapshot));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_header);
    RUN_TEST(test_roundtrip_bytes);
    RUN_TEST(test_encode_decode);
    RUN_TEST(test_fits_espnow);
    return UNITY_END();
}
