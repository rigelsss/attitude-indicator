// Testes do atraso estimado dos pacotes (lib/delay_estimator).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "delay_estimator.h"

void setUp() {}
void tearDown() {}

static const DelayConfig CFG = {1000, 10};
static const uint64_t B0 = 500000;   // relógio do B (ms), com offset arbitrário em relação ao do A

// Chegadas em dia: atraso zero
static void test_regular_zero() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 200; i++) TEST_ASSERT_EQUAL_UINT32(0, d.feed(1000 + 20 * i, B0 + 20 * i));
}

// Um pacote atrasado 35 ms e o seguinte em dia
static void test_single_late() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 10; i++) d.feed(1000 + 20 * i, B0 + 20 * i);
    TEST_ASSERT_EQUAL_UINT32(35, d.feed(1200, B0 + 200 + 35));
    TEST_ASSERT_EQUAL_UINT32(0, d.feed(1220, B0 + 220));
}

// Rajada: pacotes presos por ~1 s saem juntos (como na rodada de distância de 23/10/2026)
static void test_burst() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 50; i++) d.feed(1000 + 20 * i, B0 + 20 * i);
    // 50 pacotes gerados de 2000 a 2980 ms chegam todos entre B0+1990 e B0+2039
    uint32_t first = 0, last = 0;
    for (uint32_t i = 0; i < 50; i++) {
        const uint32_t delay = d.feed(2000 + 20 * i, B0 + 1990 + i);
        if (i == 0) first = delay;
        last = delay;
    }
    TEST_ASSERT_EQUAL_UINT32(990, first);   // o mais antigo da rajada esperou ~1 s
    TEST_ASSERT_EQUAL_UINT32(59, last);     // o mais novo devia chegar em B0+1980 e chegou em B0+2039
}

// Deriva lenta: o relógio do B anda 40 ppm mais rápido. A base acompanha pela janela, e o atraso não cresce
static void test_clock_drift() {
    DelayEstimator d(CFG);
    uint32_t maxDelay = 0;
    for (uint32_t i = 0; i < 50 * 600; i++) {   // 10 min a 50 Hz
        const uint32_t tA = 1000 + 20 * i;
        const uint64_t tB = B0 + 20ULL * i + (20ULL * i * 40) / 1000000;   // +40 ppm
        const uint32_t delay = d.feed(tA, tB);
        if (delay > maxDelay) maxDelay = delay;
    }
    TEST_ASSERT_TRUE(maxDelay <= 1);
}

// O relógio do B anda mais devagar (a deriva baixa o offset): o atraso continua zero
static void test_clock_drift_negative() {
    DelayEstimator d(CFG);
    uint32_t maxDelay = 0;
    for (uint32_t i = 0; i < 50 * 600; i++) {
        const uint64_t tB = B0 + 20ULL * i - (20ULL * i * 40) / 1000000;
        const uint32_t delay = d.feed(1000 + 20 * i, tB);
        if (delay > maxDelay) maxDelay = delay;
    }
    TEST_ASSERT_EQUAL_UINT32(0, maxDelay);
}

// A reiniciado: o t_ms volta a zero e o offset dá um salto; sem reset, todo pacote pareceria atrasado
static void test_reset_on_restart() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 100; i++) d.feed(60000 + 20 * i, B0 + 20 * i);
    d.reset();
    TEST_ASSERT_EQUAL_UINT32(0, d.feed(1100, B0 + 5000));
    TEST_ASSERT_EQUAL_UINT32(0, d.feed(1120, B0 + 5020));
    TEST_ASSERT_EQUAL_UINT32(30, d.feed(1140, B0 + 5070));
}

// Pacotes antigos saem da janela: um atraso fixo longo (todos atrasados por mais de 10 s) some — a limitação
// documentada
static void test_window_limitation() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 50; i++) d.feed(1000 + 20 * i, B0 + 20 * i);
    uint32_t delay = 0;
    for (uint32_t i = 50; i < 50 + 50 * 12; i++) delay = d.feed(1000 + 20 * i, B0 + 20 * i + 300);
    TEST_ASSERT_EQUAL_UINT32(0, delay);   // depois de 10 s, os 300 ms viraram a nova base
}

// Dentro da janela, o atraso fixo aparece
static void test_sustained_delay_within_window() {
    DelayEstimator d(CFG);
    for (uint32_t i = 0; i < 50; i++) d.feed(1000 + 20 * i, B0 + 20 * i);
    for (uint32_t i = 50; i < 50 + 50 * 5; i++)   // 5 s com 300 ms de atraso
        TEST_ASSERT_EQUAL_UINT32(300, d.feed(1000 + 20 * i, B0 + 20 * i + 300));
}

// Idade total: atraso na chegada + tempo desde a chegada (caso da revisão: 240 + 490 ms)
static void test_data_age() {
    TEST_ASSERT_EQUAL_UINT64(730, dataAgeMs(240, 490));
    TEST_ASSERT_TRUE(dataAgeMs(240, 490) > 250);   // atrasado, embora 240 < 250 e 490 < 500
    TEST_ASSERT_EQUAL_UINT64(5, dataAgeMs(0, 5));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_data_age);
    RUN_TEST(test_regular_zero);
    RUN_TEST(test_single_late);
    RUN_TEST(test_burst);
    RUN_TEST(test_clock_drift);
    RUN_TEST(test_clock_drift_negative);
    RUN_TEST(test_reset_on_restart);
    RUN_TEST(test_window_limitation);
    RUN_TEST(test_sustained_delay_within_window);
    return UNITY_END();
}
