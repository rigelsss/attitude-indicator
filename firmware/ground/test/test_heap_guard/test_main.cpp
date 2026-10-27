// Testes da trava de memória do WebSocket (lib/heap_guard).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "heap_guard.h"

void setUp() {}
void tearDown() {}

static const HeapGuardConfig CFG = {32, 40, 500, 250};

static void test_heap_bom_nada_acontece() {
    HeapGuard g(CFG);
    HeapClient c[2] = {{true, true, 0}, {true, true, 0}};
    TEST_ASSERT_EQUAL(HeapAction::None, g.step(50, 10000, c, 2).action);   // parados, mas com memória
    TEST_ASSERT_FALSE(g.paused());
}

static void test_derruba_o_mais_parado_e_nao_pausa() {
    HeapGuard g(CFG);
    HeapClient c[3] = {{true, true, 9800}, {true, true, 8000}, {true, true, 9000}};   // parados há 200, 2000 e 1000
    HeapDecision d = g.step(30, 10000, c, 3);
    TEST_ASSERT_EQUAL(HeapAction::Drop, d.action);
    TEST_ASSERT_EQUAL_INT(1, d.client);
    TEST_ASSERT_EQUAL_UINT32(2000, d.stalledMs);
    TEST_ASSERT_FALSE(g.paused());   // os outros seguem recebendo
}

static void test_sem_culpado_pausa_como_antes() {
    HeapGuard g(CFG);
    // Todos progredindo (último progresso há menos de 500 ms) ou sem nada pendente
    HeapClient c[3] = {{true, true, 9700}, {true, false, 0}, {false, true, 0}};
    TEST_ASSERT_EQUAL(HeapAction::Pause, g.step(30, 10000, c, 3).action);
    TEST_ASSERT_TRUE(g.paused());
    TEST_ASSERT_EQUAL(HeapAction::None, g.step(30, 10100, c, 3).action);   // já pausada
    TEST_ASSERT_EQUAL(HeapAction::None, g.step(38, 10200, c, 3).action);   // entre 32 e 40: continua pausada
    TEST_ASSERT_EQUAL(HeapAction::Resume, g.step(40, 10300, c, 3).action);
    TEST_ASSERT_FALSE(g.paused());
}

static void test_uma_derrubada_por_intervalo_e_depois_pausa() {
    HeapGuard g(CFG);
    HeapClient c[2] = {{true, true, 0}, {true, true, 0}};
    TEST_ASSERT_EQUAL(HeapAction::Drop, g.step(30, 10000, c, 2).action);
    c[0].ready = false;   // o derrubado sai
    // Dentro de 250 ms: espera a memória voltar, sem derrubar outro nem pausar
    TEST_ASSERT_EQUAL(HeapAction::None, g.step(30, 10100, c, 2).action);
    TEST_ASSERT_FALSE(g.paused());
    // Depois dos 250 ms, ainda pouca memória: o outro parado também cai
    HeapDecision d = g.step(30, 10250, c, 2);
    TEST_ASSERT_EQUAL(HeapAction::Drop, d.action);
    TEST_ASSERT_EQUAL_INT(1, d.client);
    c[1].ready = false;
    // Sem mais culpados e ainda pouca memória: pausa
    TEST_ASSERT_EQUAL(HeapAction::Pause, g.step(30, 10500, c, 2).action);
}

static void test_culpado_que_aparece_durante_a_pausa_cai() {
    HeapGuard g(CFG);
    HeapClient c[1] = {{true, true, 10000}};
    TEST_ASSERT_EQUAL(HeapAction::Pause, g.step(30, 10000, c, 1).action);
    HeapDecision d = g.step(30, 10600, c, 1);   // na pausa, ficou 600 ms sem progresso
    TEST_ASSERT_EQUAL(HeapAction::Drop, d.action);
    TEST_ASSERT_TRUE(g.paused());               // a pausa segue até a memória voltar
    TEST_ASSERT_EQUAL(HeapAction::Resume, g.step(45, 10900, c, 1).action);
}

static void test_volta_do_millis() {
    HeapGuard g(CFG);
    HeapClient c[1] = {{true, true, 0xFFFFFF00u}};   // último progresso antes da volta do millis
    HeapDecision d = g.step(30, 0x00000200u, c, 1);  // 768 ms depois
    TEST_ASSERT_EQUAL(HeapAction::Drop, d.action);
    TEST_ASSERT_EQUAL_UINT32(0x300, d.stalledMs);
}

// A regra de progresso pelas confirmações (revisão de 27/10/2026)
static void test_progresso_pelas_confirmacoes() {
    TEST_ASSERT_TRUE(heapProgressed(3000, 1500));    // confirmações chegaram
    TEST_ASSERT_TRUE(heapProgressed(3000, 0));       // tudo confirmado
    TEST_ASSERT_TRUE(heapProgressed(-1, 0));
    TEST_ASSERT_FALSE(heapProgressed(3000, 3000));   // nada confirmado
    TEST_ASSERT_FALSE(heapProgressed(1500, 3000));   // mais bytes saíram, nenhum confirmado
    TEST_ASSERT_FALSE(heapProgressed(-1, 500));      // sem consulta anterior: não dá para dizer que diminuiu
    TEST_ASSERT_FALSE(heapProgressed(3000, -1));     // sem socket
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_heap_bom_nada_acontece);
    RUN_TEST(test_derruba_o_mais_parado_e_nao_pausa);
    RUN_TEST(test_sem_culpado_pausa_como_antes);
    RUN_TEST(test_uma_derrubada_por_intervalo_e_depois_pausa);
    RUN_TEST(test_culpado_que_aparece_durante_a_pausa_cai);
    RUN_TEST(test_volta_do_millis);
    RUN_TEST(test_progresso_pelas_confirmacoes);
    return UNITY_END();
}
