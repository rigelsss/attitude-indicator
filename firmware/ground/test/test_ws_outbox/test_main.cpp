// Testes das filas do WebSocket (lib/ws_outbox).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "ws_outbox.h"

void setUp() {}
void tearDown() {}

static void test_ring_order_and_overflow() {
    OutRing<int, 3> r;
    int v = 0;
    TEST_ASSERT_TRUE(r.empty());
    TEST_ASSERT_FALSE(r.peek(v));
    TEST_ASSERT_TRUE(r.push(1));
    TEST_ASSERT_TRUE(r.push(2));
    TEST_ASSERT_TRUE(r.push(3));
    TEST_ASSERT_FALSE(r.push(4));   // cheia: o 1 sai
    TEST_ASSERT_EQUAL_size_t(3, r.size());
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_EQUAL(2, v);
    r.pop();
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_EQUAL(3, v);
    TEST_ASSERT_TRUE(r.push(5));   // dá a volta no buffer
    r.pop();
    r.pop();
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_EQUAL(5, v);
    r.clear();
    TEST_ASSERT_TRUE(r.empty());
    r.pop();   // vazia: nada acontece
    TEST_ASSERT_EQUAL_size_t(0, r.size());
}

// Peek sem pop: o que não pôde ser enviado continua na frente
static void test_ring_peek_keeps() {
    OutRing<int, 2> r;
    int v = 0;
    r.push(7);
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_EQUAL(7, v);
    TEST_ASSERT_EQUAL_size_t(1, r.size());
}

// Andamento de um comando já na fila: substituído no lugar, sem ocupar outra vaga
static void test_ring_replace_first() {
    OutRing<int, 4> r;
    r.push(10);
    r.push(21);
    r.push(30);
    TEST_ASSERT_TRUE(r.replaceFirst([](int x) { return x / 10 == 2; }, 23));
    TEST_ASSERT_FALSE(r.replaceFirst([](int x) { return x == 99; }, 1));
    TEST_ASSERT_EQUAL_size_t(3, r.size());
    int v = 0;
    r.pop();
    TEST_ASSERT_TRUE(r.peek(v));
    TEST_ASSERT_EQUAL(23, v);   // mesma posição, valor novo
}

static void test_recent_upsert_and_eviction() {
    RecentBySeq<int, 3> t;
    t.upsert(10, 1);    // seq 10: enviado
    t.upsert(11, 1);
    t.upsert(10, 3);    // seq 10: concluído, no mesmo lugar
    TEST_ASSERT_EQUAL_size_t(2, t.size());
    TEST_ASSERT_EQUAL(3, t.get(0));
    TEST_ASSERT_EQUAL(1, t.get(1));
    t.upsert(12, 5);
    t.upsert(13, 6);    // cheia: o seq 10 (mais antigo) sai
    TEST_ASSERT_EQUAL_size_t(3, t.size());
    TEST_ASSERT_EQUAL(1, t.get(0));   // seq 11
    TEST_ASSERT_EQUAL(6, t.get(2));   // seq 13
    t.upsert(11, 9);    // atualiza o 11 sem mudar a ordem
    TEST_ASSERT_EQUAL(9, t.get(0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ring_order_and_overflow);
    RUN_TEST(test_ring_peek_keeps);
    RUN_TEST(test_ring_replace_first);
    RUN_TEST(test_recent_upsert_and_eviction);
    return UNITY_END();
}
