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
    const BaroZeroResult r = evaluateZeroWindow(p, t, 45, 1.5f);
    TEST_ASSERT_EQUAL(BaroZeroCheck::Ok, r.status);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1010.17f, r.refHpa);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.3f, r.stdPa);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 24.5f, r.tempC);
}

static void test_zero_window_too_few_samples() {
    ScalarStats p, t;
    fillWindow(p, t, 30, 0.3);
    TEST_ASSERT_EQUAL(BaroZeroCheck::TooFewSamples, evaluateZeroWindow(p, t, 45, 1.5f).status);
}

static void test_zero_window_unstable() {
    ScalarStats p, t;
    fillWindow(p, t, 50, 3.0);   // placa mexida: ±3 Pa ≈ ±25 cm
    TEST_ASSERT_EQUAL(BaroZeroCheck::Unstable, evaluateZeroWindow(p, t, 45, 1.5f).status);
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
    RUN_TEST(test_scalar_stats);
    RUN_TEST(test_zero_window_ok);
    RUN_TEST(test_zero_window_too_few_samples);
    RUN_TEST(test_zero_window_unstable);
    return UNITY_END();
}
