// Testes das flags de validade da telemetria (lib/telemetry_flags) e do layout do contrato
// (telemetry.h: os static_assert de tamanho e offsets também são conferidos ao compilar no PC).
// Rodar no PC: pio test -e native
#include <string.h>
#include <unity.h>
#include "telemetry_flags.h"

void setUp() {}
void tearDown() {}

static const ValidityLimits LIM = {50, 200, 200};   // FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS

// Tudo válido: atitude, altitude e proa corrigida pelo magnetômetro
static ValidityInputs allGood() {
    ValidityInputs in;
    in.attInitialized = true;
    in.attDegraded = false;
    in.attAgeMs = 5;
    in.baroOk = true;
    in.baroZeroValid = true;
    in.altFinite = true;
    in.baroAgeMs = 40;
    in.hdgInitialized = true;
    in.magCalibrated = true;
    in.hdgMagSuspended = false;
    in.magAgeMs = 40;
    return in;
}

static void test_all_valid() {
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_ALT | VAL_HDG, telemetryValidFlags(allGood(), LIM));
}

static void test_attitude_rules() {
    ValidityInputs in = allGood();
    in.attInitialized = false;   // sem atitude: a proa também cai (depende de roll e pitch)
    TEST_ASSERT_EQUAL_HEX16(VAL_ALT, telemetryValidFlags(in, LIM));

    in = allGood();
    in.attAgeMs = 50;            // exatamente no limite: ainda vale
    TEST_ASSERT_BITS_HIGH(VAL_ATT, telemetryValidFlags(in, LIM));
    in.attAgeMs = 51;            // MPU parado
    TEST_ASSERT_EQUAL_HEX16(VAL_ALT, telemetryValidFlags(in, LIM));
    in.attAgeMs = AGE_NEVER;
    TEST_ASSERT_EQUAL_HEX16(VAL_ALT, telemetryValidFlags(in, LIM));
}

static void test_attitude_degraded_only_with_attitude() {
    ValidityInputs in = allGood();
    in.attDegraded = true;
    // A última correção da proa ainda foi aceita (hdgMagSuspended falso: o magnetômetro não leu desde a
    // saturação), mas a proa já segue só pelo giroscópio
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_ATT_DEGRADED | VAL_ALT | VAL_HDG | VAL_HDG_GYRO_ONLY,
                            telemetryValidFlags(in, LIM));
    in.attAgeMs = 100;           // atitude inválida: o qualificador some junto
    TEST_ASSERT_BITS_LOW(VAL_ATT | VAL_ATT_DEGRADED, telemetryValidFlags(in, LIM));
}

static void test_altitude_rules() {
    ValidityInputs in = allGood();
    in.baroOk = false;
    TEST_ASSERT_BITS_LOW(VAL_ALT, telemetryValidFlags(in, LIM));
    in = allGood();
    in.baroZeroValid = false;    // sem p₀ a altitude não tem referência
    TEST_ASSERT_BITS_LOW(VAL_ALT, telemetryValidFlags(in, LIM));
    in = allGood();
    in.altFinite = false;        // baroAltitudeM() devolveu NaN
    TEST_ASSERT_BITS_LOW(VAL_ALT, telemetryValidFlags(in, LIM));
    in = allGood();
    in.baroAgeMs = 200;
    TEST_ASSERT_BITS_HIGH(VAL_ALT, telemetryValidFlags(in, LIM));
    in.baroAgeMs = 201;          // leitura antiga
    TEST_ASSERT_BITS_LOW(VAL_ALT, telemetryValidFlags(in, LIM));
    // A altitude não depende da atitude
    in = allGood();
    in.attInitialized = false;
    TEST_ASSERT_BITS_HIGH(VAL_ALT, telemetryValidFlags(in, LIM));
}

static void test_heading_rules() {
    ValidityInputs in = allGood();
    in.hdgInitialized = false;
    TEST_ASSERT_BITS_LOW(VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));

    in = allGood();
    in.hdgMagSuspended = true;   // distúrbio, inclinação alta ou atitude degradada
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_ALT | VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));

    in = allGood();
    in.magAgeMs = 200;
    TEST_ASSERT_BITS_LOW(VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));
    in.magAgeMs = 201;           // magnetômetro parado: proa só pelo giroscópio
    TEST_ASSERT_BITS_HIGH(VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));
    in.magAgeMs = AGE_NEVER;
    TEST_ASSERT_BITS_HIGH(VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));

    // Sem atitude, nem proa nem o qualificador
    in.attAgeMs = AGE_NEVER;
    TEST_ASSERT_BITS_LOW(VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));
}

// Sem calibração do magnetômetro a proa pode errar dezenas de graus: VAL_HDG (e o qualificador) desligam
static void test_heading_requires_mag_calibration() {
    ValidityInputs in = allGood();
    in.magCalibrated = false;
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_ALT, telemetryValidFlags(in, LIM));
    in.hdgMagSuspended = true;
    TEST_ASSERT_BITS_LOW(VAL_HDG | VAL_HDG_GYRO_ONLY, telemetryValidFlags(in, LIM));
}

static void test_nothing_valid() {
    ValidityInputs in;
    memset(&in, 0, sizeof(in));
    in.attAgeMs = in.baroAgeMs = in.magAgeMs = AGE_NEVER;
    TEST_ASSERT_EQUAL_HEX16(0, telemetryValidFlags(in, LIM));
}

static void test_flags_fit_and_do_not_overlap() {
    const uint16_t all[] = {VAL_ATT, VAL_ATT_DEGRADED, VAL_ALT, VAL_HDG, VAL_HDG_GYRO_ONLY};
    uint16_t acc = 0;
    for (uint16_t f : all) {
        TEST_ASSERT_EQUAL_UINT16(0, acc & f);
        acc |= f;
    }
}

// O pacote atravessa uma cópia byte a byte (como o memcpy do B a partir do buffer do ESP-NOW) sem perder
// nada, e os campos novos ficam onde o contrato diz
static void test_packet_roundtrip_bytes() {
    TelemetryPacket p;
    memset(&p, 0, sizeof(p));
    p.hdr.magic = TELEMETRY_MAGIC;
    p.hdr.version = TELEMETRY_VERSION;
    p.hdr.type = MSG_TELEMETRY;
    p.hdr.seq = 0xBEEF;
    p.roll = -12.5f;
    p.heading = 271.25f;
    p.valid = VAL_ATT | VAL_HDG;
    p.status = ST_MPU_OK | ST_MAG_CAL;
    uint8_t buf[sizeof(TelemetryPacket) + 1];
    memcpy(buf + 1, &p, sizeof(p));   // buffer de recepção desalinhado de propósito
    TelemetryPacket q;
    memcpy(&q, buf + 1, sizeof(q));
    TEST_ASSERT_EQUAL_UINT8(TELEMETRY_VERSION, q.hdr.version);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, q.hdr.seq);
    TEST_ASSERT_EQUAL_FLOAT(-12.5f, q.roll);
    TEST_ASSERT_EQUAL_FLOAT(271.25f, q.heading);
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_HDG, q.valid);
    TEST_ASSERT_EQUAL_HEX16(ST_MPU_OK | ST_MAG_CAL, q.status);
    // valid em 50 e 51, little-endian
    TEST_ASSERT_EQUAL_HEX8(VAL_ATT | VAL_HDG, buf[1 + 50]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[1 + 51]);
}

// ACK: result e cmd_seq atravessam um buffer desalinhado nos offsets do contrato (9 e 10)
static void test_ack_roundtrip_bytes() {
    AckPacket a;
    memset(&a, 0, sizeof(a));
    a.hdr.magic = TELEMETRY_MAGIC;
    a.hdr.version = TELEMETRY_VERSION;
    a.hdr.type = MSG_ACK;
    a.hdr.seq = 7;
    a.cmd = CMD_ZERO_ALTITUDE;
    a.result = ACK_DONE;
    a.cmd_seq = 0x1234;
    uint8_t buf[sizeof(AckPacket) + 3];
    memcpy(buf + 3, &a, sizeof(a));
    AckPacket b;
    memcpy(&b, buf + 3, sizeof(b));
    TEST_ASSERT_EQUAL_UINT8(3, b.hdr.version);
    TEST_ASSERT_EQUAL_UINT8(CMD_ZERO_ALTITUDE, b.cmd);
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, b.result);
    TEST_ASSERT_EQUAL_HEX16(0x1234, b.cmd_seq);
    TEST_ASSERT_EQUAL_HEX8(ACK_DONE, buf[3 + 9]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[3 + 10]);   // little-endian
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[3 + 11]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_all_valid);
    RUN_TEST(test_attitude_rules);
    RUN_TEST(test_attitude_degraded_only_with_attitude);
    RUN_TEST(test_altitude_rules);
    RUN_TEST(test_heading_rules);
    RUN_TEST(test_heading_requires_mag_calibration);
    RUN_TEST(test_nothing_valid);
    RUN_TEST(test_flags_fit_and_do_not_overlap);
    RUN_TEST(test_packet_roundtrip_bytes);
    RUN_TEST(test_ack_roundtrip_bytes);
    return UNITY_END();
}
