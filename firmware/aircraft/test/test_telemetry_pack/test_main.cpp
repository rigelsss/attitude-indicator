// Testes da montagem do TelemetryPacket e da decodificação do CommandPacket (lib/telemetry_pack).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <math.h>
#include <string.h>
#include "telemetry_pack.h"

void setUp() {}
void tearDown() {}

static const ValidityLimits LIM = {50, 200, 200};

// Estado com tudo válido, amostras recentes em relação a NOW_MS / NOW_US
static const uint32_t NOW_MS = 100000;
static const uint64_t NOW_US = 100000000ULL;

static TelemetryState goodState() {
    TelemetryState s;
    memset(&s, 0, sizeof(s));
    s.attInitialized = true;
    s.rollDeg = -1.5f;
    s.pitchDeg = 0.2f;
    s.haveAttSample = true;
    s.attSampleUs64 = NOW_US - 3000;
    s.hdgInitialized = true;
    s.headingDeg = 96.5f;
    s.gx = 0.1f;
    s.gy = -0.2f;
    s.gz = 0.3f;
    s.baroOk = true;
    s.baroZeroValid = true;
    s.haveBaro = true;
    s.baroMs = NOW_MS - 30;
    s.altitudeM = 0.25f;
    s.pressureHpa = 1009.3f;
    s.tempC = 25.9f;
    s.magOk = true;
    s.magCalibrated = true;
    s.haveMag = true;
    s.magMs = NOW_MS - 30;
    s.mpuOk = true;
    s.gyroCalibrated = true;
    s.loopUs = 180;
    return s;
}

// Cabeçalho, campos copiados e reservas em 0
static void test_build_header_and_fields() {
    const TelemetryPacket p = buildTelemetry(goodState(), NOW_MS, NOW_US, 1234, LIM);
    TEST_ASSERT_EQUAL_HEX8(TELEMETRY_MAGIC, p.hdr.magic);
    TEST_ASSERT_EQUAL_UINT8(TELEMETRY_VERSION, p.hdr.version);
    TEST_ASSERT_EQUAL_UINT8(MSG_TELEMETRY, p.hdr.type);
    TEST_ASSERT_EQUAL_UINT8(0, p.hdr.reserved0);
    TEST_ASSERT_EQUAL_UINT16(1234, p.hdr.seq);
    TEST_ASSERT_EQUAL_UINT16(0, p.hdr.reserved1);
    TEST_ASSERT_EQUAL_UINT16(0, p.reserved2);
    TEST_ASSERT_EQUAL_UINT32(NOW_MS, p.t_ms);
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, p.roll);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, p.pitch);
    TEST_ASSERT_EQUAL_FLOAT(96.5f, p.heading);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, p.gz);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, p.altitude_m);
    TEST_ASSERT_EQUAL_FLOAT(1009.3f, p.pressure_hpa);
    TEST_ASSERT_EQUAL_FLOAT(25.9f, p.temp_c);
    TEST_ASSERT_EQUAL_UINT16(180, p.loop_us);
    TEST_ASSERT_EQUAL_HEX16(VAL_ATT | VAL_ALT | VAL_HDG, p.valid);   // 0x000D, como no c
}

// seq de 16 bits: quem chama incrementa; 65535 e depois 0 saem como estão
static void test_build_seq_wraps() {
    uint16_t seq = 65535;
    TEST_ASSERT_EQUAL_UINT16(65535, buildTelemetry(goodState(), NOW_MS, NOW_US, seq++, LIM).hdr.seq);
    TEST_ASSERT_EQUAL_UINT16(0, buildTelemetry(goodState(), NOW_MS, NOW_US, seq++, LIM).hdr.seq);
}

// Cada bit do status
static void test_status_flags() {
    TelemetryState s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL_HEX16(0, telemetryStatusFlags(s));
    s.mpuOk = true;
    TEST_ASSERT_EQUAL_HEX16(ST_MPU_OK, telemetryStatusFlags(s));
    s.magOk = true;
    s.baroOk = true;
    s.calibrating = true;
    s.gyroCalibrated = true;
    s.magCalibrated = true;
    TEST_ASSERT_EQUAL_HEX16(ST_MPU_OK | ST_MAG_OK | ST_BARO_OK | ST_CALIBRATING | ST_GYRO_CAL | ST_MAG_CAL,
                            telemetryStatusFlags(s));
}

// Idades vencidas desligam as flags; sem amostra nenhuma, AGE_NEVER
static void test_valid_ages() {
    TelemetryState s = goodState();
    s.attSampleUs64 = NOW_US - 60000;   // 60 ms > 50 ms
    s.baroMs = NOW_MS - 250;          // > 200 ms
    TelemetryPacket p = buildTelemetry(s, NOW_MS, NOW_US, 0, LIM);
    TEST_ASSERT_EQUAL_HEX16(0, p.valid & (VAL_ATT | VAL_ALT | VAL_HDG));
    s = goodState();
    s.haveMag = false;
    const ValidityInputs in = validityAt(s, NOW_MS, NOW_US);
    TEST_ASSERT_EQUAL_UINT32(AGE_NEVER, in.magAgeMs);
    TEST_ASSERT_EQUAL_UINT32(3, in.attAgeMs);
    TEST_ASSERT_EQUAL_UINT32(30, in.baroAgeMs);
}

// Virada do millis() entre a amostra e a montagem; o relógio de 64 bits da atitude passa dos 32 bits sem virar
static void test_valid_ages_wraparound() {
    TelemetryState s = goodState();
    s.attSampleUs64 = 0xFFFFF000ULL;   // 4 096 µs antes do fim dos 32 bits
    s.baroMs = 0xFFFFFFF0u;            // 16 ms antes da virada do millis()
    s.magMs = 0xFFFFFFF0u;
    const ValidityInputs in = validityAt(s, 4, 0x100000200ULL);
    TEST_ASSERT_EQUAL_UINT32(4, in.attAgeMs);    // 4 608 µs
    TEST_ASSERT_EQUAL_UINT32(20, in.baroAgeMs);
    TEST_ASSERT_EQUAL_UINT32(20, in.magAgeMs);
}

// O caso da revisão: o MPU parou, e uma volta inteira do micros() de 32 bits depois (~71,6 min) a idade em 32
// bits voltaria a dar 3 ms, religando o VAL_ATT com um ângulo velho. Em 64 bits, a idade continua enorme
static void test_stale_attitude_after_32bit_wrap() {
    TelemetryState s = goodState();
    const uint64_t t0 = 1000000ULL;
    s.attSampleUs64 = t0;
    const uint64_t now = t0 + 0x100000000ULL + 3000;   // 2³² µs + 3 ms depois
    const ValidityInputs in = validityAt(s, NOW_MS, now);
    TEST_ASSERT_TRUE(in.attAgeMs > 4000000);   // ~4 295 s
    const TelemetryPacket p = buildTelemetry(s, NOW_MS, now, 0, LIM);
    TEST_ASSERT_EQUAL_HEX16(0, p.valid & VAL_ATT);
    TEST_ASSERT_EQUAL_HEX16(0, p.valid & VAL_HDG);
    // Instante no futuro (não deveria acontecer): idade 0, sem estourar
    TEST_ASSERT_EQUAL_UINT32(0, validityAt(s, NOW_MS, t0 - 1).attAgeMs);
}

// Altitude não finita: sem VAL_ALT (o B mostra "---")
static void test_nan_altitude_clears_alt() {
    TelemetryState s = goodState();
    s.altitudeM = NAN;
    const TelemetryPacket p = buildTelemetry(s, NOW_MS, NOW_US, 0, LIM);
    TEST_ASSERT_EQUAL_HEX16(0, p.valid & VAL_ALT);
    TEST_ASSERT_TRUE(p.valid & VAL_ATT);
}

// loop_us de 16 bits: satura em 65535
static void test_loop_us_saturates() {
    TelemetryState s = goodState();
    s.loopUs = 70000;
    TEST_ASSERT_EQUAL_UINT16(65535, buildTelemetry(s, NOW_MS, NOW_US, 0, LIM).loop_us);
}

static CommandPacket validCommand() {
    CommandPacket c;
    memset(&c, 0, sizeof(c));
    c.hdr.magic = TELEMETRY_MAGIC;
    c.hdr.version = TELEMETRY_VERSION;
    c.hdr.type = MSG_COMMAND;
    c.hdr.seq = 7;
    c.cmd = CMD_ZERO_ALTITUDE;
    c.arg = -3;
    return c;
}

static void test_decode_command() {
    CommandPacket in = validCommand(), out;
    // Buffer desalinhado (o do ESP-NOW não tem alinhamento garantido): a cópia não depende dele
    uint8_t buf[sizeof(CommandPacket) + 1];
    memcpy(buf + 1, &in, sizeof(in));
    TEST_ASSERT_TRUE(decodeCommand(buf + 1, sizeof(in), out) == CmdDecode::Ok);
    TEST_ASSERT_EQUAL_UINT16(7, out.hdr.seq);
    TEST_ASSERT_EQUAL_UINT8(CMD_ZERO_ALTITUDE, out.cmd);
    TEST_ASSERT_EQUAL_INT32(-3, out.arg);

    TEST_ASSERT_TRUE(decodeCommand(buf + 1, sizeof(in) - 1, out) == CmdDecode::BadSize);
    TEST_ASSERT_TRUE(decodeCommand(nullptr, sizeof(in), out) == CmdDecode::BadSize);
    CommandPacket c = validCommand();
    c.hdr.magic = 0x00;
    TEST_ASSERT_TRUE(decodeCommand((const uint8_t *)&c, sizeof(c), out) == CmdDecode::BadMagic);
    c = validCommand();
    c.hdr.version = TELEMETRY_VERSION + 1;
    TEST_ASSERT_TRUE(decodeCommand((const uint8_t *)&c, sizeof(c), out) == CmdDecode::BadVersion);
    c = validCommand();
    c.hdr.type = MSG_TELEMETRY;
    TEST_ASSERT_TRUE(decodeCommand((const uint8_t *)&c, sizeof(c), out) == CmdDecode::BadType);
    TEST_ASSERT_EQUAL_STRING("outra versão do contrato", cmdDecodeText(CmdDecode::BadVersion));
}

// ACK: cabeçalho de ACK, campos e reservas em 0; 12 bytes
static void test_build_ack() {
    const AckPacket a = buildAck(77, CMD_ZERO_ALTITUDE, ACK_DONE, 4321);
    TEST_ASSERT_EQUAL_HEX8(TELEMETRY_MAGIC, a.hdr.magic);
    TEST_ASSERT_EQUAL_UINT8(TELEMETRY_VERSION, a.hdr.version);
    TEST_ASSERT_EQUAL_UINT8(MSG_ACK, a.hdr.type);
    TEST_ASSERT_EQUAL_UINT8(0, a.hdr.reserved0);
    TEST_ASSERT_EQUAL_UINT16(0, a.hdr.reserved1);
    TEST_ASSERT_EQUAL_UINT16(77, a.hdr.seq);
    TEST_ASSERT_EQUAL_UINT8(CMD_ZERO_ALTITUDE, a.cmd);
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    TEST_ASSERT_EQUAL_UINT16(4321, a.cmd_seq);
    TEST_ASSERT_EQUAL_size_t(12, sizeof(a));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_build_header_and_fields);
    RUN_TEST(test_build_seq_wraps);
    RUN_TEST(test_status_flags);
    RUN_TEST(test_valid_ages);
    RUN_TEST(test_valid_ages_wraparound);
    RUN_TEST(test_stale_attitude_after_32bit_wrap);
    RUN_TEST(test_nan_altitude_clears_alt);
    RUN_TEST(test_loop_us_saturates);
    RUN_TEST(test_decode_command);
    RUN_TEST(test_build_ack);
    return UNITY_END();
}
