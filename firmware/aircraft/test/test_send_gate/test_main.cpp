// Testes da regra de um envio ESP-NOW em voo por vez (lib/send_gate).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "send_gate.h"

void setUp() {}
void tearDown() {}

static void test_send_then_callback() {
    SendGate g(100, 1000);
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(0));
    g.sent(0);
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(20));
    g.callback();
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(40));
}

static void test_send_refused() {
    SendGate g(100, 1000);
    g.sent(0);
    g.sendFailed();
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(20));
}

// O tempo limite não libera um novo envio: só conta o episódio, uma vez
static void test_timeout_does_not_release() {
    SendGate g(100, 1000);
    g.sent(0);
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(99));
    TEST_ASSERT_EQUAL_UINT32(0, g.timeouts());
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(100));
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(500));
    TEST_ASSERT_EQUAL_UINT32(1, g.timeouts());
    // O callback atrasado chega: libera normalmente, sem dois em voo
    g.callback();
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(520));
    TEST_ASSERT_EQUAL_UINT32(0, g.unexpectedCallbacks());
}

// Sequência da revisão: tempo limite → novo envio → callback antigo. Com a regra, o novo envio só sai depois da
// recuperação, que descarta o antigo; um callback fora de hora não libera nada
static void test_timeout_new_send_old_callback() {
    SendGate g(100, 1000);
    g.sent(0);
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(150));   // tempo limite: ainda não envia
    TEST_ASSERT_EQUAL(GateAction::Recover, g.poll(1000));
    g.recovered();   // ESP-NOW reiniciado: o envio antigo e o callback dele foram descartados
    TEST_ASSERT_EQUAL_UINT32(1, g.recoveries());
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(1020));
    g.sent(1020);
    TEST_ASSERT_TRUE(g.inFlight());
    // Mesmo que um callback chegasse aqui, ele seria do envio novo (o antigo não existe mais depois do deinit)
    g.callback();
    TEST_ASSERT_FALSE(g.inFlight());
    // Um callback a mais, sem envio em voo: contado e ignorado
    g.callback();
    TEST_ASSERT_EQUAL_UINT32(1, g.unexpectedCallbacks());
    TEST_ASSERT_EQUAL(GateAction::Send, g.poll(1040));
}

// Cada episódio conta um tempo limite
static void test_timeout_counted_per_episode() {
    SendGate g(100, 1000);
    g.sent(0);
    g.poll(200);
    g.callback();
    g.sent(300);
    g.poll(450);
    g.poll(500);
    TEST_ASSERT_EQUAL_UINT32(2, g.timeouts());
}

// Relógio dando a volta em 32 bits
static void test_wraparound() {
    SendGate g(100, 1000);
    g.sent(0xFFFFFFF0u);
    TEST_ASSERT_EQUAL(GateAction::Skip, g.poll(0x10));   // 32 ms depois
    TEST_ASSERT_EQUAL(GateAction::Recover, g.poll(0x3F0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_then_callback);
    RUN_TEST(test_send_refused);
    RUN_TEST(test_timeout_does_not_release);
    RUN_TEST(test_timeout_new_send_old_callback);
    RUN_TEST(test_timeout_counted_per_episode);
    RUN_TEST(test_wraparound);
    return UNITY_END();
}
