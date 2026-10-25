// Testes dos comandos do B para o A (lib/command_link).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "command_link.h"
#include "telemetry.h"

void setUp() {}
void tearDown() {}

static const CmdLinkConfig CFG = {300, 3, 200000, 2000};

// Imediato: enviado, DONE; nada mais a enviar
static void test_immediate_done() {
    CommandLink l(CFG, 1000);
    uint16_t seq;
    TEST_ASSERT_TRUE(l.submit(CMD_ZERO_ATTITUDE, 0, 7, 0, seq));
    TEST_ASSERT_EQUAL_UINT16(1000, seq);
    CmdSend s;
    TEST_ASSERT_TRUE(l.nextSend(0, s));
    TEST_ASSERT_EQUAL_UINT16(1000, s.seq);
    TEST_ASSERT_EQUAL_UINT8(1, s.attempt);
    TEST_ASSERT_FALSE(l.nextSend(10, s));
    CmdEvent ev;
    TEST_ASSERT_TRUE(l.onAck(1000, CMD_ZERO_ATTITUDE, ACK_DONE, 20, ev));
    TEST_ASSERT_EQUAL(CmdState::Done, ev.state);
    TEST_ASSERT_EQUAL_UINT8(7, ev.origin);
    TEST_ASSERT_FALSE(l.nextSend(400, s));   // concluído: sem reenvio
    TEST_ASSERT_FALSE(l.onAck(1000, CMD_ZERO_ATTITUDE, ACK_DONE, 30, ev));   // repetido
}

// Sem ACK: reenvia a cada 300 ms até 3 envios, depois NoResponse
static void test_retries_and_no_response() {
    CommandLink l(CFG, 1);
    uint16_t seq;
    l.submit(CMD_SET_RATE_HZ, 25, 0, 0, seq);
    CmdSend s;
    CmdEvent ev;
    TEST_ASSERT_TRUE(l.nextSend(0, s));
    TEST_ASSERT_FALSE(l.nextSend(299, s));
    TEST_ASSERT_TRUE(l.nextSend(300, s));
    TEST_ASSERT_EQUAL_UINT8(2, s.attempt);
    TEST_ASSERT_EQUAL_INT32(25, s.arg);
    TEST_ASSERT_TRUE(l.nextSend(600, s));
    TEST_ASSERT_EQUAL_UINT8(3, s.attempt);
    TEST_ASSERT_FALSE(l.nextSend(900, s));   // não há 4º envio
    TEST_ASSERT_FALSE(l.nextTimeout(899, ev));
    TEST_ASSERT_TRUE(l.nextTimeout(900, ev));
    TEST_ASSERT_EQUAL(CmdState::NoResponse, ev.state);
    TEST_ASSERT_FALSE(l.onAck(seq, CMD_SET_RATE_HZ, ACK_DONE, 950, ev));   // atrasado: ignorado
}

// Demorado: ACCEPTED para o reenvio; DONE depois; ACCEPTED repetido ignorado
static void test_long_command() {
    CommandLink l(CFG, 50);
    uint16_t seq;
    l.submit(CMD_CALIBRATE_GYRO, 0, 1, 0, seq);
    CmdSend s;
    CmdEvent ev;
    l.nextSend(0, s);
    TEST_ASSERT_TRUE(l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_ACCEPTED, 40, ev));
    TEST_ASSERT_EQUAL(CmdState::Accepted, ev.state);
    TEST_ASSERT_FALSE(l.nextSend(400, s));   // aceito: sem reenvio
    TEST_ASSERT_FALSE(l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_ACCEPTED, 50, ev));
    TEST_ASSERT_TRUE(l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_DONE, 6000, ev));
    TEST_ASSERT_EQUAL(CmdState::Done, ev.state);
}

// Aceito sem conclusão: NoCompletion no prazo
static void test_no_completion() {
    CommandLink l(CFG, 9);
    uint16_t seq;
    l.submit(CMD_CALIBRATE_MAG, 0, 0, 0, seq);
    CmdSend s;
    CmdEvent ev;
    l.nextSend(0, s);
    l.onAck(seq, CMD_CALIBRATE_MAG, ACK_ACCEPTED, 100, ev);
    TEST_ASSERT_FALSE(l.nextTimeout(200099, ev));
    TEST_ASSERT_TRUE(l.nextTimeout(200100, ev));
    TEST_ASSERT_EQUAL(CmdState::NoCompletion, ev.state);
}

// Recusado: sem reenvio; ACK de outro seq ou de outro comando é ignorado
static void test_rejected_and_unknown() {
    CommandLink l(CFG, 100);
    uint16_t seq;
    l.submit(CMD_SET_RATE_HZ, 30, 0, 0, seq);
    CmdSend s;
    CmdEvent ev;
    l.nextSend(0, s);
    TEST_ASSERT_FALSE(l.onAck(seq + 1, CMD_SET_RATE_HZ, ACK_REJECTED, 10, ev));
    TEST_ASSERT_FALSE(l.onAck(seq, CMD_ZERO_ATTITUDE, ACK_REJECTED, 10, ev));
    TEST_ASSERT_TRUE(l.onAck(seq, CMD_SET_RATE_HZ, ACK_REJECTED, 10, ev));
    TEST_ASSERT_EQUAL(CmdState::Rejected, ev.state);
    TEST_ASSERT_FALSE(l.nextSend(400, s));
}

// Lugares: até SLOTS em curso; um terminado libera o lugar; seq sobe e dá a volta
static void test_slots_and_seq() {
    CommandLink l(CFG, 65534);
    uint16_t seqs[CommandLink::SLOTS];
    for (int i = 0; i < CommandLink::SLOTS; i++) TEST_ASSERT_TRUE(l.submit(CMD_ZERO_ATTITUDE, 0, 0, 0, seqs[i]));
    TEST_ASSERT_EQUAL_UINT16(65534, seqs[0]);
    TEST_ASSERT_EQUAL_UINT16(65535, seqs[1]);
    TEST_ASSERT_EQUAL_UINT16(0, seqs[2]);
    uint16_t extra;
    TEST_ASSERT_FALSE(l.submit(CMD_ZERO_ATTITUDE, 0, 0, 0, extra));
    CmdEvent ev;
    l.onAck(seqs[1], CMD_ZERO_ATTITUDE, ACK_DONE, 5, ev);
    TEST_ASSERT_TRUE(l.submit(CMD_ZERO_ATTITUDE, 0, 0, 5, extra));
    TEST_ASSERT_EQUAL_UINT16(2, extra);
}

// Aceito: reconsulta a cada 2 s com o mesmo seq; um DONE perdido é recuperado na consulta seguinte
static void test_poll_recovers_lost_done() {
    CommandLink l(CFG, 70);
    uint16_t seq;
    l.submit(CMD_CALIBRATE_GYRO, 0, 2, 0, seq);
    CmdSend s;
    CmdEvent ev;
    l.nextSend(0, s);
    l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_ACCEPTED, 40, ev);
    TEST_ASSERT_FALSE(l.nextSend(2039, s));
    TEST_ASSERT_TRUE(l.nextSend(2040, s));   // reconsulta
    TEST_ASSERT_EQUAL_UINT16(seq, s.seq);
    TEST_ASSERT_EQUAL_UINT8(0, s.attempt);
    TEST_ASSERT_EQUAL_UINT8(2, s.origin);
    TEST_ASSERT_FALSE(l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_ACCEPTED, 2080, ev));   // ainda em curso: sem evento
    // O DONE de ~4 s se perdeu; a consulta de 4040 o recupera
    TEST_ASSERT_TRUE(l.nextSend(4040, s));
    TEST_ASSERT_TRUE(l.onAck(seq, CMD_CALIBRATE_GYRO, ACK_DONE, 4080, ev));
    TEST_ASSERT_EQUAL(CmdState::Done, ev.state);
    TEST_ASSERT_FALSE(l.nextSend(6040, s));   // concluído: sem mais consultas
}

static void test_build_packet() {
    const CommandPacket p = buildCommandPacket({CMD_SET_RATE_HZ, 25, 4242, 1});
    TEST_ASSERT_EQUAL_HEX8(TELEMETRY_MAGIC, p.hdr.magic);
    TEST_ASSERT_EQUAL_UINT8(TELEMETRY_VERSION, p.hdr.version);
    TEST_ASSERT_EQUAL_UINT8(MSG_COMMAND, p.hdr.type);
    TEST_ASSERT_EQUAL_UINT16(4242, p.hdr.seq);
    TEST_ASSERT_EQUAL_UINT8(CMD_SET_RATE_HZ, p.cmd);
    TEST_ASSERT_EQUAL_INT32(25, p.arg);
    TEST_ASSERT_EQUAL_UINT8(0, p.reserved[0]);
    TEST_ASSERT_EQUAL_STRING("mudar taxa", commandText(CMD_SET_RATE_HZ));
}

static void test_state_text() {
    TEST_ASSERT_EQUAL_STRING("concluído", cmdStateText(CmdState::Done));
    TEST_ASSERT_EQUAL_STRING("sem resposta", cmdStateText(CmdState::NoResponse));
    TEST_ASSERT_TRUE(cmdStateFinal(CmdState::Rejected));
    TEST_ASSERT_FALSE(cmdStateFinal(CmdState::Accepted));
}

// O rid do pedido é guardado e repetido no envio, nos reenvios, nos ACKs e no tempo limite
static void test_rid_in_every_event() {
    CommandLink l(CFG, 100);
    uint16_t seq;
    TEST_ASSERT_TRUE(l.submit(CMD_ZERO_ATTITUDE, 0, 3, 0, seq, 4242));
    CmdSend s;
    TEST_ASSERT_TRUE(l.nextSend(0, s));
    TEST_ASSERT_EQUAL_UINT16(4242, s.rid);
    TEST_ASSERT_TRUE(l.nextSend(CFG.retryMs, s));   // reenvio
    TEST_ASSERT_EQUAL_UINT16(4242, s.rid);
    CmdEvent ev;
    TEST_ASSERT_TRUE(l.onAck(seq, CMD_ZERO_ATTITUDE, ACK_ACCEPTED, CFG.retryMs + 10, ev));
    TEST_ASSERT_EQUAL_UINT16(4242, ev.rid);
    TEST_ASSERT_TRUE(l.nextSend(CFG.retryMs + 10 + CFG.pollMs, s));   // reconsulta
    TEST_ASSERT_EQUAL_UINT16(4242, s.rid);
    TEST_ASSERT_TRUE(l.nextTimeout(CFG.retryMs + 10 + CFG.completionMs, ev));
    TEST_ASSERT_EQUAL(CmdState::NoCompletion, ev.state);
    TEST_ASSERT_EQUAL_UINT16(4242, ev.rid);
    // Sem rid (console, encoder, página): 0
    TEST_ASSERT_TRUE(l.submit(CMD_ZERO_ALTITUDE, 0, 0, 0, seq));
    while (l.nextSend(0, s) && s.cmd != CMD_ZERO_ALTITUDE) {}
    TEST_ASSERT_EQUAL_UINT16(0, s.rid);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_immediate_done);
    RUN_TEST(test_retries_and_no_response);
    RUN_TEST(test_long_command);
    RUN_TEST(test_no_completion);
    RUN_TEST(test_rejected_and_unknown);
    RUN_TEST(test_slots_and_seq);
    RUN_TEST(test_poll_recovers_lost_done);
    RUN_TEST(test_build_packet);
    RUN_TEST(test_state_text);
    RUN_TEST(test_rid_in_every_event);
    return UNITY_END();
}
