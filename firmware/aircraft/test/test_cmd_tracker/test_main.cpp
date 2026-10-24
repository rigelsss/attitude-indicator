// Testes do estado dos comandos do rádio (lib/cmd_tracker).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "cmd_tracker.h"
#include "telemetry.h"

void setUp() {}
void tearDown() {}

static const uint32_t LONG_MS = 210000;  // retenção do resultado de um comando demorado (janela de reconsulta do B)
static const uint32_t SHORT_MS = 3000;   // e de um imediato (inclusive as recusas)
#define RET LONG_MS, SHORT_MS

// Comando imediato: novo, concluído, e o reenvio do mesmo seq devolve o DONE sem executar de novo
static void test_immediate_and_duplicate() {
    CmdTracker t(RET);
    CmdAck a;
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(10, CMD_ZERO_ATTITUDE, 0, a));
    const CmdAck done = t.finish(10, CMD_ZERO_ATTITUDE, ACK_DONE, 0);
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, done.result);
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(10, CMD_ZERO_ATTITUDE, 300, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    TEST_ASSERT_EQUAL_UINT16(10, a.cmdSeq);
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(11, CMD_ZERO_ATTITUDE, 300, a));
}

// Demorado: ACCEPTED, consulta durante (ACCEPTED), DONE, consulta depois (DONE)
static void test_long_command() {
    CmdTracker t(RET);
    CmdAck a;
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(20, CMD_CALIBRATE_GYRO, 0, a));
    const CmdAck acc = t.start(20, CMD_CALIBRATE_GYRO, 0, 30000);
    TEST_ASSERT_EQUAL_UINT8(ACK_ACCEPTED, acc.result);
    TEST_ASSERT_TRUE(t.pending());
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(20, CMD_CALIBRATE_GYRO, 2000, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_ACCEPTED, a.result);
    const CmdAck done = t.complete(ACK_DONE, 4000);
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, done.result);
    TEST_ASSERT_FALSE(t.pending());
    // O DONE se perdeu: a reconsulta seguinte o recupera
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(20, CMD_CALIBRATE_GYRO, 6000, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
}

// Outro comando com um demorado em curso: recusado; o pendente continua e termina com o seq dele
static void test_busy() {
    CmdTracker t(RET);
    CmdAck a;
    t.arrive(30, CMD_CALIBRATE_MAG, 0, a);
    t.start(30, CMD_CALIBRATE_MAG, 0, 185000);
    TEST_ASSERT_EQUAL(CmdArrival::Busy, t.arrive(31, CMD_ZERO_ALTITUDE, 10, a));
    t.finish(31, CMD_ZERO_ALTITUDE, ACK_REJECTED, 10);
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(31, CMD_ZERO_ALTITUDE, 300, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_REJECTED, a.result);
    const CmdAck done = t.complete(ACK_DONE, 1000);
    TEST_ASSERT_EQUAL_UINT16(30, done.cmdSeq);
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(31, CMD_ZERO_ALTITUDE, 1200, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_REJECTED, a.result);
}

static void test_timeout() {
    CmdTracker t(RET);
    t.start(40, CMD_ZERO_ALTITUDE, 1000, 15000);
    TEST_ASSERT_FALSE(t.timedOut(15999));
    TEST_ASSERT_TRUE(t.timedOut(16000));
    t.complete(ACK_REJECTED, 16000);
    TEST_ASSERT_FALSE(t.timedOut(99999));
}

// Caso da revisão: o ACK do primeiro se perde e outros quatro terminam antes do reenvio. Com a retenção por
// tempo, o reenvio do primeiro continua sendo duplicado (não restaura a taxa antiga)
static void test_retention_covers_retry_window() {
    CmdTracker t(RET);
    CmdAck a;
    t.arrive(1, CMD_SET_RATE_HZ, 0, a);
    t.finish(1, CMD_SET_RATE_HZ, ACK_DONE, 0);   // taxa 25 Hz; o ACK se perde
    for (uint16_t s = 2; s <= 5; s++) {
        TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(s, CMD_SET_RATE_HZ, 100, a));
        t.finish(s, CMD_SET_RATE_HZ, ACK_DONE, 100);
    }
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(1, CMD_SET_RATE_HZ, 300, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    // Depois da retenção do imediato (3 s, além dos reenvios do B), o seq volta a ser novo
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(1, CMD_SET_RATE_HZ, 2999, a));
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(1, CMD_SET_RATE_HZ, 3000, a));
}

// Capacidade: com todos os lugares retidos, o comando novo é recusado (Full) sem apagar nenhum resultado
static void test_capacity_full() {
    CmdTracker t(RET);
    CmdAck a;
    for (uint16_t s = 0; s < CmdTracker::CAPACITY; s++) {
        TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(s, CMD_ZERO_ATTITUDE, s, a));
        t.finish(s, CMD_ZERO_ATTITUDE, ACK_DONE, s);
    }
    TEST_ASSERT_EQUAL_INT(CmdTracker::CAPACITY, t.retained());
    TEST_ASSERT_EQUAL(CmdArrival::Full, t.arrive(100, CMD_ZERO_ATTITUDE, 500, a));
    t.finish(100, CMD_ZERO_ATTITUDE, ACK_REJECTED, 500);   // não guardado
    TEST_ASSERT_EQUAL_INT(CmdTracker::CAPACITY, t.retained());
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(0, CMD_ZERO_ATTITUDE, 500, a));   // o mais antigo continua
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    // O primeiro expira: abre um lugar
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(101, CMD_ZERO_ATTITUDE, 3000, a));
}

// Com um demorado em curso, um lugar fica reservado para o resultado dele
static void test_capacity_reserves_pending_result() {
    CmdTracker t(RET);
    CmdAck a;
    for (uint16_t s = 0; s < CmdTracker::CAPACITY - 2; s++) {
        t.arrive(s, CMD_ZERO_ATTITUDE, 0, a);
        t.finish(s, CMD_ZERO_ATTITUDE, ACK_DONE, 0);
    }
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(50, CMD_CALIBRATE_GYRO, 0, a));   // 2 livres
    t.start(50, CMD_CALIBRATE_GYRO, 0, 30000);
    TEST_ASSERT_EQUAL(CmdArrival::Busy, t.arrive(51, CMD_ZERO_ALTITUDE, 10, a));   // 2 livres: recusa guardada
    t.finish(51, CMD_ZERO_ALTITUDE, ACK_REJECTED, 10);
    // Full com 1 livre (o reservado): a recusa é respondida com refuse(), sem ocupar o lugar
    TEST_ASSERT_EQUAL(CmdArrival::Full, t.arrive(52, CMD_ZERO_ALTITUDE, 20, a));
    const CmdAck r = t.refuse(52, CMD_ZERO_ALTITUDE);
    TEST_ASSERT_EQUAL_UINT8(ACK_REJECTED, r.result);
    TEST_ASSERT_EQUAL_UINT16(52, r.cmdSeq);
    TEST_ASSERT_EQUAL_INT(CmdTracker::CAPACITY - 1, t.retained());   // o lugar reservado continua livre
    // O demorado termina antes de qualquer expiração: o resultado dele cabe e é recuperável
    t.complete(ACK_DONE, 1000);
    TEST_ASSERT_EQUAL_INT(CmdTracker::CAPACITY, t.retained());
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(50, CMD_CALIBRATE_GYRO, 1100, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    // A recusa não foi guardada: o reenvio do 52 é avaliado de novo (e, com o histórico cheio, recusado de novo)
    TEST_ASSERT_EQUAL(CmdArrival::Full, t.arrive(52, CMD_ZERO_ALTITUDE, 1200, a));
}

// O resultado de um demorado fica 30 s (o B o reconsulta); o de um imediato, 3 s
static void test_retention_by_kind() {
    CmdTracker t(RET);
    CmdAck a;
    t.arrive(60, CMD_CALIBRATE_GYRO, 0, a);
    t.start(60, CMD_CALIBRATE_GYRO, 0, 30000);
    t.complete(ACK_DONE, 4000);
    t.arrive(61, CMD_ZERO_ATTITUDE, 4000, a);
    t.finish(61, CMD_ZERO_ATTITUDE, ACK_DONE, 4000);
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(60, CMD_CALIBRATE_GYRO, 20000, a));
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(61, CMD_ZERO_ATTITUDE, 20000, a));
    // Link fora por mais de 30 s depois do DONE: a reconsulta tardia ainda recebe o DONE, sem executar de novo
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(60, CMD_CALIBRATE_GYRO, 4000 + 199000, a));
    TEST_ASSERT_EQUAL_UINT8(ACK_DONE, a.result);
    TEST_ASSERT_EQUAL(CmdArrival::Duplicate, t.arrive(60, CMD_CALIBRATE_GYRO, 4000 + 209999, a));
    TEST_ASSERT_EQUAL(CmdArrival::New, t.arrive(60, CMD_CALIBRATE_GYRO, 4000 + 210000, a));
}

// Bancada de 23/10/2026: a tecla g repetida ~4 vezes por segundo durante uma calibração. As recusas (imediatas)
// expiram em 3 s e não lotam o histórico: nenhuma vira Full
static void test_repeated_key_does_not_fill() {
    CmdTracker t(RET);
    CmdAck a;
    t.arrive(100, CMD_CALIBRATE_GYRO, 0, a);
    t.start(100, CMD_CALIBRATE_GYRO, 0, 30000);
    for (uint16_t i = 0; i < 40; i++) {
        const uint32_t now = 250u * (i + 1);
        TEST_ASSERT_EQUAL(CmdArrival::Busy, t.arrive(101 + i, CMD_CALIBRATE_GYRO, now, a));
        t.finish(101 + i, CMD_CALIBRATE_GYRO, ACK_REJECTED, now);
    }
    TEST_ASSERT_TRUE(t.retained() <= 13);   // ~12 recusas dentro dos 3 s
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_immediate_and_duplicate);
    RUN_TEST(test_long_command);
    RUN_TEST(test_busy);
    RUN_TEST(test_timeout);
    RUN_TEST(test_retention_covers_retry_window);
    RUN_TEST(test_capacity_full);
    RUN_TEST(test_capacity_reserves_pending_result);
    RUN_TEST(test_retention_by_kind);
    RUN_TEST(test_repeated_key_does_not_fill);
    return UNITY_END();
}
