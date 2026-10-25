// Testes dos quadros do WebSocket enviados sem esperar (lib/ws_frame).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "ws_frame.h"

void setUp() {}
void tearDown() {}

static void test_header_sizes() {
    uint8_t h[4];
    TEST_ASSERT_EQUAL_size_t(2, wsTextHeader(5, h));
    TEST_ASSERT_EQUAL_HEX8(0x81, h[0]);
    TEST_ASSERT_EQUAL_HEX8(5, h[1]);
    TEST_ASSERT_EQUAL_size_t(2, wsTextHeader(125, h));
    TEST_ASSERT_EQUAL_HEX8(125, h[1]);
    TEST_ASSERT_EQUAL_size_t(4, wsTextHeader(126, h));   // a partir de 126: tamanho estendido de 16 bits
    TEST_ASSERT_EQUAL_HEX8(126, h[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[2]);
    TEST_ASSERT_EQUAL_HEX8(126, h[3]);
    TEST_ASSERT_EQUAL_size_t(4, wsTextHeader(450, h));   // telemetria típica
    TEST_ASSERT_EQUAL_HEX8(0x01, h[2]);
    TEST_ASSERT_EQUAL_HEX8(0xC2, h[3]);
    TEST_ASSERT_EQUAL_size_t(0, wsTextHeader(65536, h));
}

static void test_load_and_full_send() {
    FrameTx<64> tx;
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_TRUE(tx.load("abc", 3));
    TEST_ASSERT_FALSE(tx.idle());
    size_t n = 0;
    const uint8_t *p = tx.pending(n);
    TEST_ASSERT_EQUAL_size_t(5, n);
    const uint8_t want[] = {0x81, 3, 'a', 'b', 'c'};
    TEST_ASSERT_EQUAL_MEMORY(want, p, 5);
    tx.consumed(5);
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_FALSE(tx.partial());
}

// O socket aceita só parte: o resto continua, e nada novo entra antes
static void test_partial_send() {
    FrameTx<64> tx;
    TEST_ASSERT_TRUE(tx.load("hello", 5));
    tx.consumed(3);
    TEST_ASSERT_TRUE(tx.partial());
    TEST_ASSERT_FALSE(tx.load("x", 1));
    size_t n = 0;
    const uint8_t *p = tx.pending(n);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_MEMORY("llo", p + 1, 3);
    tx.consumed(100);   // mais do que falta: só termina
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_TRUE(tx.load("x", 1));
}

static void test_too_big_and_reset() {
    FrameTx<8> tx;
    TEST_ASSERT_FALSE(tx.load("1234567", 7));   // 2 + 7 > 8
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_TRUE(tx.load("123456", 6));
    tx.consumed(2);
    tx.reset();   // cliente caiu: o resto do quadro vai embora
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_FALSE(tx.partial());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_header_sizes);
    RUN_TEST(test_load_and_full_send);
    RUN_TEST(test_partial_send);
    RUN_TEST(test_too_big_and_reset);
    return UNITY_END();
}
