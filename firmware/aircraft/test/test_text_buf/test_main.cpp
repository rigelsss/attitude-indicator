// Testes do acumulador de texto (lib/log_line, TextBuf). Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "text_buf.h"

void setUp() {}
void tearDown() {}

static void test_appends_in_order() {
    char b[64];
    TextBuf t(b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("", t.c_str());
    t.printf("[A] %d Hz", 200);
    t.print(" | ok");
    t.println(" fim");
    TEST_ASSERT_EQUAL_STRING("[A] 200 Hz | ok fim\r\n", t.c_str());
    TEST_ASSERT_EQUAL_UINT32(strlen("[A] 200 Hz | ok fim\r\n"), t.size());
    TEST_ASSERT_FALSE(t.truncated());
}

// Cabe exatamente (cap − 1 caracteres): não é corte
static void test_exact_fit_is_not_truncated() {
    char b[6];
    TextBuf t(b, sizeof(b));
    t.print("abc");
    t.print("de");
    TEST_ASSERT_EQUAL_STRING("abcde", t.c_str());
    TEST_ASSERT_FALSE(t.truncated());
}

// Não coube: guarda o que couber, termina em '\0' e marca o corte; acréscimos seguintes não escrevem fora
static void test_overflow_truncates_and_marks() {
    char b[8];
    TextBuf t(b, sizeof(b));
    t.print("abcd");
    t.printf("%s", "efghij");
    TEST_ASSERT_TRUE(t.truncated());
    TEST_ASSERT_EQUAL_STRING("abcdefg", t.c_str());
    TEST_ASSERT_EQUAL_UINT32(7, t.size());
    t.print("xyz");
    TEST_ASSERT_EQUAL_STRING("abcdefg", t.c_str());
    TEST_ASSERT_TRUE(t.truncated());
}

static void test_clear_resets() {
    char b[8];
    TextBuf t(b, sizeof(b));
    t.print("abcdefghij");
    t.clear();
    TEST_ASSERT_FALSE(t.truncated());
    TEST_ASSERT_EQUAL_UINT32(0, t.size());
    t.print("ok");
    TEST_ASSERT_EQUAL_STRING("ok", t.c_str());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_appends_in_order);
    RUN_TEST(test_exact_fit_is_not_truncated);
    RUN_TEST(test_overflow_truncates_and_marks);
    RUN_TEST(test_clear_resets);
    return UNITY_END();
}
