// Testes da conferência e cópia do TelemetryPacket recebido (lib/telemetry_rx).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "telemetry_rx.h"

void setUp() {}
void tearDown() {}

static TelemetryPacket goodPacket() {
    TelemetryPacket p;
    memset(&p, 0, sizeof(p));
    p.hdr.magic = TELEMETRY_MAGIC;
    p.hdr.version = TELEMETRY_VERSION;
    p.hdr.type = MSG_TELEMETRY;
    p.hdr.seq = 4321;
    p.t_ms = 123456;
    p.roll = -1.5f;
    p.pitch = 0.25f;
    p.heading = 96.5f;
    p.altitude_m = 0.75f;
    p.status = 0x0037;
    p.valid = 0x000D;
    p.loop_us = 180;
    return p;
}

// Pacote válido lido de um buffer desalinhado (offset 1), como pode chegar do ESP-NOW
static void test_decode_ok_from_unaligned_buffer() {
    const TelemetryPacket src = goodPacket();
    uint8_t raw[sizeof(TelemetryPacket) + 1];
    memcpy(raw + 1, &src, sizeof(src));
    TelemetryPacket out;
    memset(&out, 0xFF, sizeof(out));
    TEST_ASSERT_EQUAL(RxDecode::Ok, decodeTelemetry(raw + 1, sizeof(TelemetryPacket), out));
    TEST_ASSERT_EQUAL_UINT16(4321, out.hdr.seq);
    TEST_ASSERT_EQUAL_UINT32(123456, out.t_ms);
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, out.roll);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, out.pitch);
    TEST_ASSERT_EQUAL_FLOAT(96.5f, out.heading);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, out.altitude_m);
    TEST_ASSERT_EQUAL_HEX16(0x0037, out.status);
    TEST_ASSERT_EQUAL_HEX16(0x000D, out.valid);
    TEST_ASSERT_EQUAL_UINT16(180, out.loop_us);
}

static void test_decode_bad_size() {
    const TelemetryPacket p = goodPacket();
    TelemetryPacket out;
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeTelemetry(nullptr, sizeof(p), out));
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeTelemetry((const uint8_t *)&p, 0, out));
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeTelemetry((const uint8_t *)&p, sizeof(MsgHeader) - 1, out));
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeTelemetry((const uint8_t *)&p, sizeof(p) - 1, out));
    uint8_t big[sizeof(p) + 4] = {};
    memcpy(big, &p, sizeof(p));
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeTelemetry(big, sizeof(big), out));
}

static void test_decode_bad_magic() {
    TelemetryPacket p = goodPacket();
    p.hdr.magic = 0x00;
    TelemetryPacket out;
    TEST_ASSERT_EQUAL(RxDecode::BadMagic, decodeTelemetry((const uint8_t *)&p, sizeof(p), out));
}

// Outra versão é recusada pela versão, mesmo com outro tamanho
static void test_decode_bad_version() {
    TelemetryPacket p = goodPacket();
    p.hdr.version = TELEMETRY_VERSION - 1;
    TelemetryPacket out;
    TEST_ASSERT_EQUAL(RxDecode::BadVersion, decodeTelemetry((const uint8_t *)&p, sizeof(p), out));
    TEST_ASSERT_EQUAL(RxDecode::BadVersion, decodeTelemetry((const uint8_t *)&p, 40, out));
}

// Um ACK (12 bytes) do A é recusado pelo tipo, e não pelo tamanho
static void test_decode_bad_type() {
    AckPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.hdr.magic = TELEMETRY_MAGIC;
    ack.hdr.version = TELEMETRY_VERSION;
    ack.hdr.type = MSG_ACK;
    TelemetryPacket out;
    TEST_ASSERT_EQUAL(RxDecode::BadType, decodeTelemetry((const uint8_t *)&ack, sizeof(ack), out));
}

// ACK válido, de um buffer desalinhado; telemetria recusada pelo tipo
static void test_decode_ack() {
    AckPacket a;
    memset(&a, 0, sizeof(a));
    a.hdr.magic = TELEMETRY_MAGIC;
    a.hdr.version = TELEMETRY_VERSION;
    a.hdr.type = MSG_ACK;
    a.cmd = CMD_ZERO_ALTITUDE;
    a.result = ACK_DONE;
    a.cmd_seq = 777;
    uint8_t raw[sizeof(AckPacket) + 1];
    memcpy(raw + 1, &a, sizeof(a));
    AckPacket out;
    TEST_ASSERT_EQUAL(RxDecode::Ok, decodeAck(raw + 1, sizeof(AckPacket), out));
    TEST_ASSERT_EQUAL_UINT8(CMD_ZERO_ALTITUDE, out.cmd);
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, out.result);
    TEST_ASSERT_EQUAL_UINT16(777, out.cmd_seq);
    TEST_ASSERT_EQUAL_UINT8(MSG_ACK, rxMessageType(raw + 1, sizeof(AckPacket)));
    const TelemetryPacket p = goodPacket();
    TEST_ASSERT_EQUAL(RxDecode::BadType, decodeAck((const uint8_t *)&p, sizeof(p), out));
    TEST_ASSERT_EQUAL(RxDecode::BadSize, decodeAck(raw + 1, sizeof(AckPacket) - 1, out));
    TEST_ASSERT_EQUAL_UINT8(0, rxMessageType(raw, 3));
}

static void test_decode_text() {
    TEST_ASSERT_EQUAL_STRING("ok", rxDecodeText(RxDecode::Ok));
    TEST_ASSERT_EQUAL_STRING("tamanho", rxDecodeText(RxDecode::BadSize));
    TEST_ASSERT_EQUAL_STRING("tipo", rxDecodeText(RxDecode::BadType));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_decode_ok_from_unaligned_buffer);
    RUN_TEST(test_decode_bad_size);
    RUN_TEST(test_decode_bad_magic);
    RUN_TEST(test_decode_bad_version);
    RUN_TEST(test_decode_bad_type);
    RUN_TEST(test_decode_ack);
    RUN_TEST(test_decode_text);
    return UNITY_END();
}
