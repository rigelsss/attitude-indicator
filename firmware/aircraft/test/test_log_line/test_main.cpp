// Testes do montador de linhas (lib/log_line). Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "log_line.h"

void setUp() {}
void tearDown() {}

// Entrega os caracteres de s; devolve quantas vezes a linha ficou pronta e guarda a última em out
static int feed(LineAssembler &la, const char *s, char *out, size_t outCap) {
    int ready = 0;
    for (const char *p = s; *p; p++) {
        if (!la.put(*p)) continue;
        ready++;
        const size_t n = la.size() < outCap - 1 ? la.size() : outCap - 1;
        memcpy(out, la.data(), n);
        out[n] = '\0';
        la.clear();
    }
    return ready;
}

static void test_line_ends_at_newline() {
    char buf[32];
    LineAssembler la(buf, sizeof(buf));
    char out[40];
    TEST_ASSERT_EQUAL_INT(0, feed(la, "[E][Wire.cpp:499] erro", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(22, la.size());
    TEST_ASSERT_EQUAL_INT(1, feed(la, "\r\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("[E][Wire.cpp:499] erro\r\n", out);
    TEST_ASSERT_EQUAL_UINT32(0, la.size());
}

static void test_consecutive_lines() {
    char buf[16];
    LineAssembler la(buf, sizeof(buf));
    char out[20];
    TEST_ASSERT_EQUAL_INT(2, feed(la, "um\ndois\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("dois\n", out);
}

// Linha maior que o buffer: sai em pedaços de cap bytes, sem perder caracteres
static void test_long_line_split_at_capacity() {
    char buf[4];
    LineAssembler la(buf, sizeof(buf));
    char out[8];
    TEST_ASSERT_EQUAL_INT(1, feed(la, "abcd", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("abcd", out);
    TEST_ASSERT_EQUAL_INT(1, feed(la, "ef\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ef\n", out);
}

// Linha pronta sem clear(): o próximo caractere recomeça a linha, sem escrever fora do buffer
static void test_full_without_clear_restarts() {
    char buf[3];
    LineAssembler la(buf, sizeof(buf));
    TEST_ASSERT_TRUE(la.put('a') == false);
    la.put('b');
    TEST_ASSERT_TRUE(la.put('c'));
    TEST_ASSERT_FALSE(la.put('d'));
    TEST_ASSERT_EQUAL_UINT32(1, la.size());
    TEST_ASSERT_EQUAL_CHAR('d', la.data()[0]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_line_ends_at_newline);
    RUN_TEST(test_consecutive_lines);
    RUN_TEST(test_long_line_split_at_capacity);
    RUN_TEST(test_full_without_clear_restarts);
    return UNITY_END();
}
