// Testes do núcleo da fusão (lib/fusion) e da cadeia da amostra (imuToBody, lib/attitude).
// Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "fusion.h"

void setUp() {}
void tearDown() {}

// Mesmos valores do config.h (20 amostras na média inicial)
static const FusionParams FP = {1.0f, 0.05f, 0.15f, 0.3f, 0.050f, 20, 5.0f};
static const HeadingParams HP = {1.0f, 70.0f, 0.2f};
static const ImuFlags NO_SAT = {false, false};
static const Vec3 LEVEL = {0, 0, -1};   // força específica em repouso nivelado, eixos do modelo
static const Vec3 ZERO = {0, 0, 0};
static const uint32_t PERIOD_US = 5000;   // 200 Hz

static void assertVec(const Vec3 &e, const Vec3 &a, float tol) {
    TEST_ASSERT_FLOAT_WITHIN(tol, e.x, a.x);
    TEST_ASSERT_FLOAT_WITHIN(tol, e.y, a.y);
    TEST_ASSERT_FLOAT_WITHIN(tol, e.z, a.z);
}

// n amostras nivelado e parado, a cada 5 ms a partir de t0; devolve o instante da próxima
static uint32_t feedLevel(FusionCore &f, uint32_t t0, int n, bool headingHold = false) {
    for (int i = 0; i < n; i++) f.update(LEVEL, ZERO, NO_SAT, t0 + (uint32_t)i * PERIOD_US, headingHold);
    return t0 + (uint32_t)n * PERIOD_US;
}

// ---------- imuToBody ----------

// A cadeia é a mesma de antes da refatoração: calibração nos eixos do chip → chipToBody → montagem
static void test_imu_to_body_matches_chain() {
    AccelCal cal;
    cal.offset = {0.02f, -0.01f, 0.10f};
    cal.scale = {1.01f, 0.99f, 1.005f};
    const Vec3 bias = {-4.8f, 2.6f, 1.1f};
    // Montagem: 3° em torno de x
    const float c = cosf(3.0f * (float)M_PI / 180.0f), s = sinf(3.0f * (float)M_PI / 180.0f);
    Mat3 mount = mat3Identity();
    mount.m[1][1] = c;
    mount.m[1][2] = -s;
    mount.m[2][1] = s;
    mount.m[2][2] = c;
    const Vec3 aRaw = {0.09f, 0.04f, 1.10f}, gRaw = {-4.7f, 2.7f, 1.2f};

    const BodyImu b = imuToBody(aRaw, gRaw, true, cal, bias, mount);
    assertVec(mat3Apply(mount, chipToBody(applyAccelCal(cal, aRaw))), b.acc, 1e-6f);
    assertVec(mat3Apply(mount, chipToBody(applyGyroBias(gRaw, bias))), b.gyro, 1e-6f);

    // Sem calibração do acelerômetro: só eixos e montagem
    const BodyImu u = imuToBody(aRaw, gRaw, false, cal, bias, mount);
    assertVec(mat3Apply(mount, chipToBody(aRaw)), u.acc, 1e-6f);
}

// Valores esperados calculados à mão, para pegar sinal trocado ou ordem errada das transformações.
// Calibração: (raw − offset)/escala nos eixos do chip; chipToBody: (x, y, z) = (Y, X, −Z);
// montagem: 90° em torno de x, (x, y, z) → (x, −z, y).
static void test_imu_to_body_known_values() {
    AccelCal cal;
    cal.offset = {0.1f, 0.2f, 0.3f};
    cal.scale = {2.0f, 4.0f, 0.5f};
    const Vec3 bias = {1.0f, 2.0f, 4.0f};
    Mat3 mount = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
    const Vec3 aRaw = {1.1f, 0.6f, 0.8f}, gRaw = {3.0f, 5.0f, 7.0f};

    // a: chip calibrado (0,5; 0,1; 1,0) → modelo (0,1; 0,5; −1,0) → montagem (0,1; 1,0; 0,5)
    // g: chip sem bias (2; 3; 3) → modelo (3; 2; −3) → montagem (3; 3; 2)
    const BodyImu b = imuToBody(aRaw, gRaw, true, cal, bias, mount);
    assertVec({0.1f, 1.0f, 0.5f}, b.acc, 1e-5f);
    assertVec({3.0f, 3.0f, 2.0f}, b.gyro, 1e-5f);

    // Sem calibração do acelerômetro: (1,1; 0,6; 0,8) → modelo (0,6; 1,1; −0,8) → montagem (0,6; 0,8; 1,1)
    const BodyImu u = imuToBody(aRaw, gRaw, false, cal, bias, mount);
    assertVec({0.6f, 0.8f, 1.1f}, u.acc, 1e-5f);
    assertVec({3.0f, 3.0f, 2.0f}, u.gyro, 1e-5f);

    // Sem montagem (identidade): só calibração e eixos
    const BodyImu n = imuToBody(aRaw, gRaw, true, cal, bias, mat3Identity());
    assertVec({0.1f, 0.5f, -1.0f}, n.acc, 1e-5f);
    assertVec({3.0f, 2.0f, -3.0f}, n.gyro, 1e-5f);
}

// ---------- FusionCore ----------

// A primeira amostra marca a referência do dt (dt = 0) e já entra na média inicial de 20
static void test_first_sample_sets_reference() {
    FusionCore f(FP, HP);
    TEST_ASSERT_FALSE(f.havePrev());
    TEST_ASSERT_TRUE(f.update(LEVEL, ZERO, NO_SAT, 1000, false) == AttitudeFilter::Step::Waiting);
    TEST_ASSERT_TRUE(f.havePrev());
    TEST_ASSERT_EQUAL_UINT32(1000, f.prevIntUs());
    feedLevel(f, 1000 + PERIOD_US, 18);
    TEST_ASSERT_FALSE(f.att().initialized());
    TEST_ASSERT_TRUE(f.update(LEVEL, ZERO, NO_SAT, 1000 + 19 * PERIOD_US, false) ==
                     AttitudeFilter::Step::Initialized);
    TEST_ASSERT_TRUE(f.att().initialized());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, f.att().rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, f.att().pitchDeg());
}

// dt pelos instantes dos pulsos, inclusive na virada do micros()
static void test_dt_from_pulses_and_wraparound() {
    FusionCore f(FP, HP);
    const uint32_t t0 = 0xFFFFFFFFu - 50u * PERIOD_US;   // a virada cai no meio das amostras
    const uint32_t next = feedLevel(f, t0, 100);
    TEST_ASSERT_TRUE(f.att().initialized());
    const FusionStats &st = f.stats();
    TEST_ASSERT_TRUE(st.n > 0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.005f, st.dtMin);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.005f, st.dtMax);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.005, st.dtSum / st.n);
    TEST_ASSERT_EQUAL_UINT32(next - PERIOD_US, f.prevIntUs());
    TEST_ASSERT_EQUAL_UINT32(0, st.resets);
}

// Lacuna acima de maxDtS: o filtro reinicia e o reinício entra na estatística
static void test_gap_counts_reset() {
    FusionCore f(FP, HP);
    uint32_t t = feedLevel(f, 0, 30);
    TEST_ASSERT_TRUE(f.att().initialized());
    t += 60000;   // 60 ms sem amostras
    TEST_ASSERT_TRUE(f.update(LEVEL, ZERO, NO_SAT, t, false) == AttitudeFilter::Step::Reset);
    TEST_ASSERT_EQUAL_UINT32(1, f.stats().resets);
    TEST_ASSERT_FALSE(f.att().initialized());
}

static void test_reset_clears_everything() {
    FusionCore f(FP, HP);
    feedLevel(f, 0, 30);
    f.heading().correct(100.0f, false, 0.0f, 0.04f);
    TEST_ASSERT_TRUE(f.att().initialized());
    TEST_ASSERT_TRUE(f.gyroOnly().initialized());
    TEST_ASSERT_TRUE(f.heading().initialized());
    f.reset();
    TEST_ASSERT_FALSE(f.att().initialized());
    TEST_ASSERT_FALSE(f.gyroOnly().initialized());
    TEST_ASSERT_FALSE(f.heading().initialized());
    TEST_ASSERT_FALSE(f.havePrev());
}

// O diagnóstico só de giroscópio parte da atitude do filtro e depois só integra
static void test_gyro_only_starts_from_filter() {
    FusionCore f(FP, HP);
    uint32_t t = feedLevel(f, 0, 19);
    TEST_ASSERT_FALSE(f.att().initialized());
    TEST_ASSERT_FALSE(f.gyroOnly().initialized());
    t = feedLevel(f, t, 1);   // a 20ª inicializa o filtro, e na mesma amostra o diagnóstico copia a atitude
    TEST_ASSERT_TRUE(f.att().initialized());
    TEST_ASSERT_TRUE(f.gyroOnly().initialized());
    // 1 s girando a 10 °/s em roll: o diagnóstico integra os 10°; o filtro fica perto de 0 (acelerômetro)
    for (int i = 0; i < 200; i++, t += PERIOD_US) f.update(LEVEL, {10, 0, 0}, NO_SAT, t, false);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, f.gyroOnly().rollDeg());
    TEST_ASSERT_TRUE(fabsf(f.att().rollDeg()) < 7.0f);
    f.resetGyroOnly();
    TEST_ASSERT_FALSE(f.gyroOnly().initialized());
    TEST_ASSERT_TRUE(f.att().initialized());
}

// Proa: predição a cada amostra; parada e reiniciada durante a calibração do magnetômetro
static void test_heading_predict_and_hold() {
    FusionCore f(FP, HP);
    uint32_t t = feedLevel(f, 0, 30);
    f.heading().correct(100.0f, false, 0.0f, 0.04f);
    TEST_ASSERT_TRUE(f.heading().initialized());
    // 1 s de guinada a 20 °/s (eixo z do modelo, para baixo: nariz para a direita)
    for (int i = 0; i < 200; i++, t += PERIOD_US) f.update(LEVEL, {0, 0, 20}, NO_SAT, t, false);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 120.0f, f.heading().headingDeg());
    f.update(LEVEL, ZERO, NO_SAT, t, true);
    TEST_ASSERT_FALSE(f.heading().initialized());
}

// Sem atitude, a proa recomeça
static void test_heading_resets_without_attitude() {
    FusionCore f(FP, HP);
    f.heading().correct(100.0f, false, 0.0f, 0.04f);
    f.update(LEVEL, ZERO, NO_SAT, 0, false);
    TEST_ASSERT_FALSE(f.heading().initialized());
}

// Custo do filtro medido pelo relógio, só nas amostras Updated
static uint32_t g_fakeUs = 0;
static uint32_t fakeClock() { return g_fakeUs += 7; }

static void test_stats_cost_and_reset() {
    FusionCore f(FP, HP);
    FusionCore noClock(FP, HP);
    f.setClock(fakeClock);
    feedLevel(f, 0, 40);
    feedLevel(noClock, 0, 40);
    const FusionStats &st = f.stats();
    TEST_ASSERT_TRUE(st.n > 0);
    TEST_ASSERT_EQUAL_UINT32(7 * st.n, st.costSumUs);
    TEST_ASSERT_EQUAL_UINT32(7, st.costMaxUs);
    TEST_ASSERT_EQUAL_UINT32(0, noClock.stats().costSumUs);
    TEST_ASSERT_EQUAL_UINT32(st.n, noClock.stats().n);
    f.resetStats();
    TEST_ASSERT_EQUAL_UINT32(0, f.stats().n);
    TEST_ASSERT_EQUAL_UINT32(0, f.stats().costMaxUs);
    TEST_ASSERT_TRUE(f.att().initialized());   // a estatística é só do fluxo a: o filtro continua
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_imu_to_body_matches_chain);
    RUN_TEST(test_imu_to_body_known_values);
    RUN_TEST(test_first_sample_sets_reference);
    RUN_TEST(test_dt_from_pulses_and_wraparound);
    RUN_TEST(test_gap_counts_reset);
    RUN_TEST(test_reset_clears_everything);
    RUN_TEST(test_gyro_only_starts_from_filter);
    RUN_TEST(test_heading_predict_and_hold);
    RUN_TEST(test_heading_resets_without_attitude);
    RUN_TEST(test_stats_cost_and_reset);
    return UNITY_END();
}
