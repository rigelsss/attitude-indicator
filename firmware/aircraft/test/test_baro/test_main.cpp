// Testes do barômetro em lógica pura (lib/baro). Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "baro.h"

void setUp() {}
void tearDown() {}

// Coeficientes sintéticos com valores típicos de um BME280/BMP280 (não são do nosso chip)
static const Bme280Calib CAL = {27504, 26435, -1000, 36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000};

// Referência: fórmulas em ponto flutuante do datasheet (seção 8.1), com t_fine truncado como no original
static void referenceDouble(const Bme280Calib &c, int32_t adcT, int32_t adcP, double &tC, double &pPa) {
    double v1 = (adcT / 16384.0 - c.T1 / 1024.0) * c.T2;
    double v2 = (adcT / 131072.0 - c.T1 / 8192.0) * (adcT / 131072.0 - c.T1 / 8192.0) * c.T3;
    const int32_t tFine = (int32_t)(v1 + v2);
    tC = (v1 + v2) / 5120.0;
    v1 = tFine / 2.0 - 64000.0;
    v2 = v1 * v1 * c.P6 / 32768.0;
    v2 = v2 + v1 * c.P5 * 2.0;
    v2 = v2 / 4.0 + c.P4 * 65536.0;
    v1 = (c.P3 * v1 * v1 / 524288.0 + c.P2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * c.P1;
    double p = 1048576.0 - adcP;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = c.P9 * p * p / 2147483648.0;
    v2 = p * c.P8 / 32768.0;
    pPa = p + (v1 + v2 + c.P7) / 16.0;
}

static void test_parse_calib_little_endian_and_sign() {
    // T1 = 0x6B70 (27504), T2 = 0x6743 (26435), T3 = 0xFC18 (−1000), P1 = 0x8E7D (36477), P2 = 0xD643 (−10685)
    uint8_t raw[BME280_CALIB_TP_LEN] = {0x70, 0x6B, 0x43, 0x67, 0x18, 0xFC, 0x7D, 0x8E, 0x43, 0xD6};
    raw[22] = 0x70;   // P9 = 0x1770 (6000)
    raw[23] = 0x17;
    Bme280Calib c;
    TEST_ASSERT_TRUE(bme280ParseCalib(raw, c));
    TEST_ASSERT_EQUAL_UINT16(27504, c.T1);
    TEST_ASSERT_EQUAL_INT16(26435, c.T2);
    TEST_ASSERT_EQUAL_INT16(-1000, c.T3);
    TEST_ASSERT_EQUAL_UINT16(36477, c.P1);
    TEST_ASSERT_EQUAL_INT16(-10685, c.P2);
    TEST_ASSERT_EQUAL_INT16(6000, c.P9);
}

static void test_parse_calib_rejects_zero_coefficients() {
    uint8_t raw[BME280_CALIB_TP_LEN] = {0};
    Bme280Calib c;
    TEST_ASSERT_FALSE(bme280ParseCalib(raw, c));
}

static void test_parse_adc_20_bits() {
    const uint8_t raw[6] = {0x65, 0x5A, 0xC0, 0x7E, 0xED, 0x00};
    int32_t p, t;
    bme280ParseAdc(raw, p, t);
    TEST_ASSERT_EQUAL_INT32(415148, p);
    TEST_ASSERT_EQUAL_INT32(519888, t);
}

static void test_compensation_plausible() {
    float t, p;
    TEST_ASSERT_EQUAL(BaroComp::Ok, bme280Compensate(CAL, 519888, 415148, t, p));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 25.0f, t);
    TEST_ASSERT_FLOAT_WITHIN(20.0f, 1006.0f, p);
}

// A versão inteira tem que concordar com a versão em ponto flutuante do datasheet em toda a faixa
static void test_compensation_matches_double_formulas() {
    float worstT = 0, worstP = 0;
    for (int32_t adcT = 400000; adcT <= 600000; adcT += 5000) {
        for (int32_t adcP = 250000; adcP <= 500000; adcP += 5000) {
            float t, p;
            TEST_ASSERT_EQUAL(BaroComp::Ok, bme280Compensate(CAL, adcT, adcP, t, p));
            double tRef, pRef;
            referenceDouble(CAL, adcT, adcP, tRef, pRef);
            worstT = fmaxf(worstT, (float)fabs(t - tRef));
            worstP = fmaxf(worstP, (float)fabs(p * 100.0 - pRef));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(worstT <= 0.01f, "temperatura: diferença acima de 0,01 °C");
    TEST_ASSERT_TRUE_MESSAGE(worstP <= 1.0f, "pressão: diferença acima de 1 Pa");
}

static void test_compensation_skipped() {
    float t = 0, p = 0;
    TEST_ASSERT_EQUAL(BaroComp::Skipped, bme280Compensate(CAL, BME280_ADC_SKIPPED, 415148, t, p));
    TEST_ASSERT_EQUAL(BaroComp::Skipped, bme280Compensate(CAL, 519888, BME280_ADC_SKIPPED, t, p));
}

static void test_altitude_zero_at_reference() {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, baroAltitudeM(1010.17f, 1010.17f));
}

static void test_altitude_one_hpa_near_sea_level() {
    // 1 hPa a menos perto de 1013,25 hPa ≈ +8,3 m
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 8.33f, baroAltitudeM(1012.25f, 1013.25f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -8.33f, baroAltitudeM(1014.25f, 1013.25f));
}

static void test_altitude_bench_meter() {
    // Bancada (hardware.md): ~12 Pa a menos em 1010 hPa ≈ 1 m de subida
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.00f, baroAltitudeM(1009.88f, 1010.0f));
}

static void test_altitude_invalid_inputs() {
    // Pressão ou referência não finitas, nulas ou negativas: NaN, e não um número qualquer
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(0.0f, 1010.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(-5.0f, 1010.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(NAN, 1010.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(INFINITY, 1010.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(1010.0f, 0.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(1010.0f, -1010.0f)));
    TEST_ASSERT_TRUE(isnan(baroAltitudeM(1010.0f, NAN)));
}

static void test_scalar_stats() {
    ScalarStats s;
    s.reset();
    const double v[] = {2, 4, 4, 4, 5, 5, 7, 9};
    for (double x : v) s.add(x);
    TEST_ASSERT_EQUAL_UINT32(8, s.count());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 5.0f, (float)s.mean());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, (float)s.stddev());
}

// Janela sintética: pressão em torno de 101017 Pa com oscilação ±amp, temperatura fixa
static void fillWindow(ScalarStats &p, ScalarStats &t, uint32_t n, double ampPa) {
    p.reset();
    t.reset();
    for (uint32_t i = 0; i < n; i++) {
        p.add(101017.0 + ((i % 2) ? ampPa : -ampPa));
        t.add(24.5);
    }
}

static void test_zero_window_ok() {
    ScalarStats p, t;
    fillWindow(p, t, 50, 0.3);
    const BaroZeroResult r = evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f);
    TEST_ASSERT_EQUAL(BaroZeroCheck::Ok, r.status);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1010.17f, r.refHpa);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.3f, r.stdPa);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.5f, r.tempC);
}

static void test_zero_window_too_few_samples() {
    ScalarStats p, t;
    fillWindow(p, t, 30, 0.3);
    TEST_ASSERT_EQUAL(BaroZeroCheck::TooFewSamples, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
}

static void test_zero_window_unstable() {
    ScalarStats p, t;
    fillWindow(p, t, 50, 3.0);   // placa mexida: ±3 Pa ≈ ±25 cm
    TEST_ASSERT_EQUAL(BaroZeroCheck::Unstable, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
}

// Uma leitura não finita contamina média e desvio; antes, a janela era aceita com p0 = NaN
static void test_zero_window_rejects_non_finite() {
    ScalarStats p, t;
    fillWindow(p, t, 50, 0.3);
    p.add(NAN);
    TEST_ASSERT_EQUAL(BaroZeroCheck::Invalid, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
    fillWindow(p, t, 50, 0.3);
    t.add(NAN);
    TEST_ASSERT_EQUAL(BaroZeroCheck::Invalid, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
}

// Janela estável, mas com pressão absurda (leituras zeradas ou fora da faixa do chip): não vira p0
static void test_zero_window_out_of_range() {
    ScalarStats p, t;
    for (int i = 0; i < 50; i++) {
        p.add(0.0);
        t.add(24.5);
    }
    TEST_ASSERT_EQUAL(BaroZeroCheck::OutOfRange, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
    p.reset();
    for (int i = 0; i < 50; i++) p.add(125000.0);   // 1250 hPa
    TEST_ASSERT_EQUAL(BaroZeroCheck::OutOfRange, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
    // Nos limites, aceita
    p.reset();
    for (int i = 0; i < 50; i++) p.add(30000.0);
    TEST_ASSERT_EQUAL(BaroZeroCheck::Ok, evaluateZeroWindow(p, t, 45, 1.5f, 300.0f, 1100.0f).status);
}

// ---------- Máquina de estado do zero (BaroZero) ----------

static const BaroZeroParams ZP = {2000, 2000, 45, 1.5f, 300.0f, 1100.0f};

struct FeedOut {
    BaroZero::Event last;   // último evento diferente de None
    int events;
    uint32_t tEvent;
};

// Leituras a 25 Hz (a cada 40 ms) de t0 até t1, inclusive; a pressão alterna ±ampPa em torno de hpa
static FeedOut feedRange(BaroZero &z, uint32_t t0, uint32_t t1, float hpa, float ampPa) {
    FeedOut o = {BaroZero::Event::None, 0, 0};
    int i = 0;
    for (uint32_t t = t0; (int32_t)(t1 - t) >= 0; t += 40, i++) {
        const BaroZero::Event e = z.feed(hpa + ((i % 2) ? ampPa : -ampPa) / 100.0f, 24.5f, t);
        if (e != BaroZero::Event::None) {
            o.last = e;
            o.events++;
            o.tEvent = t;
        }
    }
    return o;
}

static void test_zero_off_ignores_samples() {
    BaroZero z(ZP);
    TEST_ASSERT_TRUE(z.feed(1010.0f, 24.5f, 0) == BaroZero::Event::None);
    TEST_ASSERT_FALSE(z.active());
    TEST_ASSERT_FALSE(z.refValid());
}

// Boot: espera o filtro do chip (2 s) e mede 2 s; janela de 51 leituras, como na bancada
static void test_zero_settle_then_accept() {
    BaroZero z(ZP);
    z.start(true, 1000);
    TEST_ASSERT_TRUE(z.state() == BaroZero::State::Settling);
    TEST_ASSERT_EQUAL(0, feedRange(z, 1000, 2960, 1010.17f, 0.3f).events);
    TEST_ASSERT_TRUE(z.state() == BaroZero::State::Settling);
    TEST_ASSERT_EQUAL(0, feedRange(z, 3000, 4960, 1010.17f, 0.3f).events);
    TEST_ASSERT_TRUE(z.state() == BaroZero::State::Measuring);
    TEST_ASSERT_TRUE(z.feed(1010.17f, 24.5f, 5000) == BaroZero::Event::Accepted);
    TEST_ASSERT_EQUAL_UINT32(51, z.lastCount());
    TEST_ASSERT_TRUE(z.lastResult().status == BaroZeroCheck::Ok);
    TEST_ASSERT_FALSE(z.active());
    TEST_ASSERT_TRUE(z.refValid());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1010.17f, z.refHpa());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 24.5f, z.refTempC());
}

// Tecla 0: mede logo, sem a espera
static void test_zero_without_settle() {
    BaroZero z(ZP);
    z.start(false, 0);
    const FeedOut o = feedRange(z, 0, 2000, 1010.17f, 0.3f);
    TEST_ASSERT_EQUAL(1, o.events);
    TEST_ASSERT_TRUE(o.last == BaroZero::Event::Accepted);
    TEST_ASSERT_EQUAL_UINT32(2000, o.tEvent);
    TEST_ASSERT_EQUAL_UINT32(51, z.lastCount());
}

// Recusada, a janela recomeça na última leitura (que não entra na nova janela)
static void test_zero_unstable_restarts_window() {
    BaroZero z(ZP);
    z.start(false, 0);
    const FeedOut o = feedRange(z, 0, 2000, 1010.17f, 3.0f);
    TEST_ASSERT_TRUE(o.last == BaroZero::Event::Rejected);
    TEST_ASSERT_TRUE(z.lastResult().status == BaroZeroCheck::Unstable);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 3.0f, z.lastResult().stdPa);
    TEST_ASSERT_TRUE(z.state() == BaroZero::State::Measuring);
    TEST_ASSERT_FALSE(z.refValid());
    TEST_ASSERT_EQUAL(0, feedRange(z, 2040, 3960, 1010.17f, 0.3f).events);
    TEST_ASSERT_TRUE(z.feed(1010.17f, 24.5f, 4000) == BaroZero::Event::Accepted);
    TEST_ASSERT_EQUAL_UINT32(50, z.lastCount());
}

static void test_zero_too_few_samples() {
    BaroZero z(ZP);
    z.start(false, 0);
    TEST_ASSERT_TRUE(z.feed(1010.0f, 24.5f, 0) == BaroZero::Event::None);
    TEST_ASSERT_TRUE(z.feed(1010.0f, 24.5f, 1000) == BaroZero::Event::None);
    TEST_ASSERT_TRUE(z.feed(1010.0f, 24.5f, 2000) == BaroZero::Event::Rejected);
    TEST_ASSERT_TRUE(z.lastResult().status == BaroZeroCheck::TooFewSamples);
    TEST_ASSERT_EQUAL_UINT32(3, z.lastCount());
}

static void test_zero_invalid_sample() {
    BaroZero z(ZP);
    z.start(false, 0);
    z.feed(NAN, 24.5f, 0);
    const FeedOut o = feedRange(z, 40, 2000, 1010.17f, 0.3f);
    TEST_ASSERT_TRUE(o.last == BaroZero::Event::Rejected);
    TEST_ASSERT_TRUE(z.lastResult().status == BaroZeroCheck::Invalid);
}

static void test_zero_out_of_range() {
    BaroZero z(ZP);
    z.start(false, 0);
    TEST_ASSERT_TRUE(feedRange(z, 0, 2000, 200.0f, 0.3f).last == BaroZero::Event::Rejected);
    TEST_ASSERT_TRUE(z.lastResult().status == BaroZeroCheck::OutOfRange);
    TEST_ASSERT_FALSE(z.refValid());
}

// Um novo zero não apaga o anterior até ser aceito
static void test_zero_new_zero_keeps_reference() {
    BaroZero z(ZP);
    z.start(false, 0);
    feedRange(z, 0, 2000, 1010.17f, 0.3f);
    z.start(false, 3000);
    TEST_ASSERT_TRUE(z.active());
    TEST_ASSERT_TRUE(z.refValid());
    TEST_ASSERT_TRUE(feedRange(z, 3000, 4960, 1009.0f, 3.0f).events == 0);
    TEST_ASSERT_TRUE(z.feed(1009.0f, 24.5f, 5000) == BaroZero::Event::Rejected);   // instável
    TEST_ASSERT_TRUE(z.refValid());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1010.17f, z.refHpa());
    TEST_ASSERT_TRUE(feedRange(z, 5040, 7000, 1009.0f, 0.3f).last == BaroZero::Event::Accepted);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1009.0f, z.refHpa());
}

// Chip reconfigurado no meio da janela: recomeça com a espera, e as leituras antigas não contam
static void test_zero_restart_discards_window() {
    BaroZero z(ZP);
    z.start(false, 0);
    feedRange(z, 0, 1000, 1010.17f, 0.3f);
    z.start(true, 1000);
    TEST_ASSERT_TRUE(z.state() == BaroZero::State::Settling);
    TEST_ASSERT_EQUAL(0, feedRange(z, 1040, 4960, 1010.17f, 0.3f).events);
    TEST_ASSERT_TRUE(z.feed(1010.17f, 24.5f, 5000) == BaroZero::Event::Accepted);
    TEST_ASSERT_EQUAL_UINT32(51, z.lastCount());
}

// Virada do millis() (~49,7 dias) durante a espera e a janela
static void test_zero_millis_wraparound() {
    BaroZero z(ZP);
    const uint32_t t0 = 0xFFFFF830u;   // 2000 ms antes da virada: a espera termina em 0
    z.start(true, t0);
    const FeedOut o = feedRange(z, t0, 2000, 1010.17f, 0.3f);
    TEST_ASSERT_EQUAL(1, o.events);
    TEST_ASSERT_TRUE(o.last == BaroZero::Event::Accepted);
    TEST_ASSERT_EQUAL_UINT32(2000, o.tEvent);
    TEST_ASSERT_EQUAL_UINT32(51, z.lastCount());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_calib_little_endian_and_sign);
    RUN_TEST(test_parse_calib_rejects_zero_coefficients);
    RUN_TEST(test_parse_adc_20_bits);
    RUN_TEST(test_compensation_plausible);
    RUN_TEST(test_compensation_matches_double_formulas);
    RUN_TEST(test_compensation_skipped);
    RUN_TEST(test_altitude_zero_at_reference);
    RUN_TEST(test_altitude_one_hpa_near_sea_level);
    RUN_TEST(test_altitude_bench_meter);
    RUN_TEST(test_altitude_invalid_inputs);
    RUN_TEST(test_scalar_stats);
    RUN_TEST(test_zero_window_ok);
    RUN_TEST(test_zero_window_too_few_samples);
    RUN_TEST(test_zero_window_unstable);
    RUN_TEST(test_zero_window_rejects_non_finite);
    RUN_TEST(test_zero_window_out_of_range);
    RUN_TEST(test_zero_off_ignores_samples);
    RUN_TEST(test_zero_settle_then_accept);
    RUN_TEST(test_zero_without_settle);
    RUN_TEST(test_zero_unstable_restarts_window);
    RUN_TEST(test_zero_too_few_samples);
    RUN_TEST(test_zero_invalid_sample);
    RUN_TEST(test_zero_out_of_range);
    RUN_TEST(test_zero_new_zero_keeps_reference);
    RUN_TEST(test_zero_restart_discards_window);
    RUN_TEST(test_zero_millis_wraparound);
    return UNITY_END();
}
