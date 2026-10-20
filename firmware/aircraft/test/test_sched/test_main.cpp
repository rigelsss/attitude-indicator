// Testes da agenda periódica (lib/sched). Rodar no PC: pio test -e native
#include <unity.h>
#include "sched.h"

void setUp() {}
void tearDown() {}

static void test_start_is_due_now() {
    Periodic p;
    p.start(1000);
    TEST_ASSERT_TRUE(p.due(1000));
    TEST_ASSERT_TRUE(p.due(1001));
    TEST_ASSERT_FALSE(p.due(999));
}

// Atendida com atraso menor que um período: a fase não muda (próxima = prevista + período)
static void test_advance_keeps_phase() {
    Periodic p;
    p.start(0);
    p.advance(300, 40000);   // atendida 0,3 ms depois do previsto
    TEST_ASSERT_EQUAL_UINT32(40000, p.next());
    TEST_ASSERT_FALSE(p.due(39999));
    TEST_ASSERT_TRUE(p.due(40000));
    p.advance(40100, 40000);
    TEST_ASSERT_EQUAL_UINT32(80000, p.next());
}

// Atraso de mais de um período (laço parado): não tenta recuperar as execuções perdidas
static void test_advance_resyncs_when_late() {
    Periodic p;
    p.start(0);
    p.advance(100000, 40000);
    TEST_ASSERT_EQUAL_UINT32(140000, p.next());
    TEST_ASSERT_FALSE(p.due(139999));
}

// Exatamente um período atrasada: a prevista + período já venceu, então ressincroniza
static void test_advance_exactly_one_period_late() {
    Periodic p;
    p.start(0);
    p.advance(40000, 40000);
    TEST_ASSERT_EQUAL_UINT32(80000, p.next());
}

// Período mais longo com o sensor falhando, e de volta ao normal
static void test_advance_period_change() {
    Periodic p;
    p.start(0);
    p.advance(10, 1000000);   // falhando: 1 s
    TEST_ASSERT_EQUAL_UINT32(1000000, p.next());
    p.advance(1000020, 40000);   // recuperou: 40 ms
    TEST_ASSERT_EQUAL_UINT32(1040000, p.next());
}

static void test_restart_from_now() {
    Periodic p;
    p.start(0);
    p.restart(12345, 100000);
    TEST_ASSERT_EQUAL_UINT32(112345, p.next());
    TEST_ASSERT_FALSE(p.due(112344));
    TEST_ASSERT_TRUE(p.due(112345));
}

// Virada do micros(): a próxima execução cai depois do zero
static void test_wraparound() {
    Periodic p;
    p.start(0xFFFFFF00u);
    TEST_ASSERT_TRUE(p.due(0xFFFFFF00u));
    p.advance(0xFFFFFF10u, 1000);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFF00u + 1000, p.next());   // 0x2E8, depois da virada
    TEST_ASSERT_FALSE(p.due(0xFFFFFFFFu));
    TEST_ASSERT_FALSE(p.due(0x2E7));
    TEST_ASSERT_TRUE(p.due(0x2E8));
    p.restart(0xFFFFFFF0u, 100);
    TEST_ASSERT_FALSE(p.due(0x53));
    TEST_ASSERT_TRUE(p.due(0x54));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_start_is_due_now);
    RUN_TEST(test_advance_keeps_phase);
    RUN_TEST(test_advance_resyncs_when_late);
    RUN_TEST(test_advance_exactly_one_period_late);
    RUN_TEST(test_advance_period_change);
    RUN_TEST(test_restart_from_now);
    RUN_TEST(test_wraparound);
    return UNITY_END();
}
