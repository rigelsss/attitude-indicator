// Testes da fusão de atitude em lógica pura (lib/attitude). Rodar no PC: pio test -e native
// Casos exatos primeiro (resposta analítica); ruído determinístico só como complemento.
#include <math.h>
#include <unity.h>
#include "attitude.h"

void setUp() {}
void tearDown() {}

// Mesmos valores do config.h. P inicializa com uma amostra só, para isolar a cinemática e a
// correção nos testes que partem de uma atitude conhecida; P_CFG usa a média de 20 amostras.
static const FusionParams P_CFG = {1.0f, 0.05f, 0.15f, 0.3f, 0.050f, 20, 5.0f};
static const FusionParams P = {1.0f, 0.05f, 0.15f, 0.3f, 0.050f, 1, 5.0f};
static const float DT = 0.005f;   // 200 Hz
static const ImuFlags NO_SAT = {false, false};
static const float D2R = (float)M_PI / 180.0f;

// Ruído determinístico: xorshift32 + aproximação gaussiana (soma de 12 uniformes − 6)
static uint32_t g_seed = 12345;
static float uniform01() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return (g_seed & 0xFFFFFF) / 16777216.0f;
}
static float gauss(float sigma) {
    float s = 0;
    for (int i = 0; i < 12; i++) s += uniform01();
    return (s - 6.0f) * sigma;
}

// Força específica em repouso nos eixos do modelo para uma atitude (roll, pitch) em graus
static Vec3 restForce(float rollDeg, float pitchDeg) {
    const float r = rollDeg * D2R, p = pitchDeg * D2R;
    return {sinf(p), -sinf(r) * cosf(p), -cosf(r) * cosf(p)};
}

// Velocidades angulares no corpo (°/s) para taxas de Euler (φ̇, θ̇, ψ̇) numa atitude (φ, θ)
static Vec3 bodyRates(float rollDeg, float pitchDeg, float rollDot, float pitchDot, float yawDot) {
    const float sr = sinf(rollDeg * D2R), cr = cosf(rollDeg * D2R);
    const float sp = sinf(pitchDeg * D2R), cp = cosf(pitchDeg * D2R);
    return {rollDot - yawDot * sp, pitchDot * cr + yawDot * sr * cp, -pitchDot * sr + yawDot * cr * cp};
}

static Mat3 mul(const Mat3 &a, const Mat3 &b) {
    Mat3 c;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            c.m[i][j] = 0;
            for (int k = 0; k < 3; k++) c.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    return c;
}
static Mat3 rotX(float deg) {
    const float c = cosf(deg * D2R), s = sinf(deg * D2R);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
static Mat3 rotY(float deg) {
    const float c = cosf(deg * D2R), s = sinf(deg * D2R);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
static Mat3 rotZ(float deg) {
    const float c = cosf(deg * D2R), s = sinf(deg * D2R);
    return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

// ---------- Mapeamento e ângulos do acelerômetro ----------

// Montagem final (hardware.md, 14/10/2026), sinais do acelerômetro confirmados na bancada: nivelada
// dá az ≈ +1 g; nariz para cima, ay ≈ +1 g; asa direita para baixo, ax ≈ −1 g. Giroscópio pela mesma
// rotação: asa direita descendo gy+ e nariz subindo gx+ (roll e pitch confirmados com o calço em
// 14/10); nariz à direita gz− (yaw ainda a confirmar no teste de giros da proa).
static void test_chip_to_body_mapping() {
    const Vec3 b = chipToBody({0.1f, 0.2f, 0.3f});
    TEST_ASSERT_EQUAL_FLOAT(0.2f, b.x);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, b.y);
    TEST_ASSERT_EQUAL_FLOAT(-0.3f, b.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, norm({0.1f, 0.2f, 0.3f}), norm(b));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, accelRollPitch(chipToBody({0, 0, 1})).rollDeg);    // face para cima
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 90.0f, accelRollPitch(chipToBody({-1, 0, 0})).rollDeg);  // asa direita p/ baixo
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 90.0f, accelRollPitch(chipToBody({0, 1, 0})).pitchDeg);  // nariz para cima
    const Vec3 g = chipToBody({+10, +10, -10});   // nariz subindo, asa direita descendo, nariz à direita
    TEST_ASSERT_TRUE(g.x > 0 && g.y > 0 && g.z > 0);
    // Rotação própria (determinante +1): os eixos do modelo continuam um sistema destro
    const Vec3 ex = chipToBody({1, 0, 0}), ey = chipToBody({0, 1, 0}), ez = chipToBody({0, 0, 1});
    const float det = ex.x * (ey.y * ez.z - ey.z * ez.y) - ex.y * (ey.x * ez.z - ey.z * ez.x) +
                      ex.z * (ey.x * ez.y - ey.y * ez.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, det);
}

static void test_accel_angles_exact() {
    const float cases[][2] = {{0, 0}, {30, 0}, {-30, 0}, {0, 20}, {0, -20}, {30, 20}, {-45, -15}, {120, 10}};
    for (const auto &c : cases) {
        const Angles a = accelRollPitch(restForce(c[0], c[1]));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[0], a.rollDeg);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[1], a.pitchDeg);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, fabsf(accelRollPitch({0, 0, 1}).rollDeg));   // de cabeça para baixo
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -170.0f, wrap180(190.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 180.0f, wrap180(-180.0f));
}

// ---------- Inicialização ----------

static void test_init_direct_at_rest() {
    const float cases[][2] = {{0, 0}, {25, -10}, {-40, 30}};
    for (const auto &c : cases) {
        AttitudeFilter f(P_CFG);
        TEST_ASSERT_FALSE(f.initialized());
        for (int i = 1; i < P_CFG.initSamples; i++)
            TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting,
                                  (int)f.update(restForce(c[0], c[1]), {0, 0, 0}, DT, NO_SAT));
        TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                              (int)f.update(restForce(c[0], c[1]), {0, 0, 0}, DT, NO_SAT));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[0], f.rollDeg());
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[1], f.pitchDeg());
        for (int i = 0; i < 200; i++) f.update(restForce(c[0], c[1]), {0, 0, 0}, DT, NO_SAT);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[0], f.rollDeg());
        TEST_ASSERT_FLOAT_WITHIN(0.01f, c[1], f.pitchDeg());
    }
}

static void test_init_waits_without_reference() {
    AttitudeFilter f(P);
    const AttitudeFilter::Step W = AttitudeFilter::Step::Waiting;
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({0, 0, -1.10f}, {0, 0, 0}, DT, NO_SAT));     // peso 0,5
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({1.0f, 0, -0.1f}, {0, 0, 0}, DT, NO_SAT));   // vertical: sem roll
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({0, 0, -1}, {0, 0, 0}, DT, {true, false}));  // acelerômetro saturado
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({0, 0, -1}, {0, 0, 0}, DT, {false, true}));  // giroscópio saturado
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({0, 0, 0}, {0, 0, 0}, DT, NO_SAT));          // vetor nulo
    TEST_ASSERT_EQUAL_INT((int)W, (int)f.update({0, 0, -1}, {6, 0, 0}, DT, NO_SAT));         // girando
    TEST_ASSERT_FALSE(f.initialized());
    // Só giroscópio: nunca se inicializa sozinho
    AttitudeFilter g(P, false);
    TEST_ASSERT_EQUAL_INT((int)W, (int)g.update({0, 0, -1}, {0, 0, 0}, DT, NO_SAT));
    g.initFrom(10, -5);
    TEST_ASSERT_TRUE(g.initialized());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, g.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -5.0f, g.pitchDeg());
}

// Média de 20 amostras com o ruído da bancada (2 mg ≈ 0,11° por amostra): o erro inicial cai
// para ~0,03°, contra ~0,1° de uma amostra só
static void test_init_averages_noise() {
    const Vec3 f0 = restForce(-1.0f, -0.3f);
    float worstAvg = 0, worstSingle = 0;
    for (int trial = 0; trial < 50; trial++) {
        AttitudeFilter avg(P_CFG), single(P);
        AttitudeFilter::Step st = AttitudeFilter::Step::Waiting;
        for (int i = 0; i < P_CFG.initSamples; i++) {
            const Vec3 a = {f0.x + gauss(0.002f), f0.y + gauss(0.002f), f0.z + gauss(0.002f)};
            if (i == 0) single.update(a, {0, 0, 0}, DT, NO_SAT);
            st = avg.update(a, {gauss(0.035f), gauss(0.035f), gauss(0.035f)}, DT, NO_SAT);
        }
        TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized, (int)st);
        worstAvg = fmaxf(worstAvg, fabsf(avg.rollDeg() + 1.0f));
        worstSingle = fmaxf(worstSingle, fabsf(single.rollDeg() + 1.0f));
    }
    TEST_ASSERT_TRUE(worstAvg < 0.1f);
    TEST_ASSERT_TRUE(worstAvg < 0.5f * worstSingle);
}

// A média só vale para amostras seguidas: uma amostra inutilizável (tranco ou giro) recomeça a contagem
static void test_init_restarts_on_bad_sample() {
    AttitudeFilter f(P_CFG);
    for (int i = 0; i < 15; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting, (int)f.update({0, 0, -1.2f}, {0, 0, 0}, DT, NO_SAT));
    for (int i = 1; i < P_CFG.initSamples; i++)
        TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting,
                              (int)f.update(restForce(10, 0), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                          (int)f.update(restForce(10, 0), {0, 0, 0}, DT, NO_SAT));
    // Só as amostras depois da interrupção entram na média
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, f.rollDeg());
    // Depois de um reset, a média recomeça do zero
    f.reset();
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting, (int)f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT));
    for (int i = 0; i < 10; i++) f.update(restForce(0, 0), {4, 0, 0}, DT, NO_SAT);   // giro lento: aceito
    TEST_ASSERT_FALSE(f.initialized());
    for (int i = 0; i < 9; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_TRUE(f.initialized());
}

// Lacuna acima do limite durante a média (leituras interrompidas e caixa movida nesse intervalo):
// a média recomeça, e só as amostras depois da lacuna contam
static void test_init_restarts_after_gap() {
    AttitudeFilter f(P_CFG);
    for (int i = 0; i < P_CFG.initSamples - 1; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting,
                          (int)f.update(restForce(30, 0), {0, 0, 0}, 1.0f, NO_SAT));
    TEST_ASSERT_FALSE(f.initialized());
    for (int i = 1; i < P_CFG.initSamples - 1; i++) f.update(restForce(30, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                          (int)f.update(restForce(30, 0), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, f.rollDeg());
}

// NaN no meio da média zera a média parcial; dt ≤ 0 no meio da média não entra nela
static void test_init_bad_data_and_zero_dt() {
    AttitudeFilter f(P_CFG);
    for (int i = 0; i < 10; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::BadData, (int)f.update({NAN, 0, -1}, {0, 0, 0}, DT, NO_SAT));
    for (int i = 1; i < P_CFG.initSamples; i++)
        TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Waiting,
                              (int)f.update(restForce(20, 0), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                          (int)f.update(restForce(20, 0), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, f.rollDeg());   // nada das amostras a 0° antes do NaN

    AttitudeFilter g(P_CFG);
    g.update(restForce(10, 0), {0, 0, 0}, 0, NO_SAT);   // primeira amostra: qualquer dt
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Skipped, (int)g.update(restForce(50, 0), {0, 0, 0}, 0, NO_SAT));
    for (int i = 2; i < P_CFG.initSamples; i++) g.update(restForce(10, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FALSE(g.initialized());
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                          (int)g.update(restForce(10, 0), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, g.rollDeg());   // a amostra a 50° com dt = 0 ficou fora
}

// ---------- Predição ----------

// Roll a 30 °/s por 2 s, com o acelerômetro coerente: segue a trajetória φ(t) = 30·t
static void test_constant_rate_roll() {
    AttitudeFilter f(P), g(P, false);
    f.update(restForce(0, 0), {0, 0, 0}, 0, NO_SAT);
    g.initFrom(0, 0);
    for (int i = 1; i <= 400; i++) {
        const float truth = 30.0f * i * DT;
        f.update(restForce(truth, 0), {30, 0, 0}, DT, NO_SAT);
        g.update(restForce(truth, 0), {30, 0, 0}, DT, NO_SAT);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, truth, f.rollDeg());
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, f.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, g.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, f.pitchDeg());
}

// Pitch a 15 °/s com roll de 20° fixo (q e r coerentes): pitch vai a 30°, roll não muda
static void test_constant_rate_pitch_with_roll() {
    AttitudeFilter g(P, false);
    g.initFrom(20, 0);
    for (int i = 1; i <= 400; i++) g.update({0, 0, -1}, bodyRates(20, 15.0f * (i - 1) * DT, 0, 15, 0), DT, NO_SAT);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, g.pitchDeg());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g.rollDeg());
}

// Caixa inclinada (roll 25°, pitch 30°) girando 360° em torno da vertical a 20 °/s: p, q e r
// variam, mas roll e pitch ficam constantes. A integração direta de p e q derivaria.
static void test_yaw_with_constant_tilt() {
    AttitudeFilter g(P, false);
    g.initFrom(25, 30);
    const Vec3 w = bodyRates(25, 30, 0, 0, 20);
    TEST_ASSERT_TRUE(fabsf(w.x) > 5 && fabsf(w.y) > 5 && fabsf(w.z) > 5);
    for (int i = 0; i < 3600; i++) g.update({0, 0, -1}, w, DT, NO_SAT);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.0f, g.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.0f, g.pitchDeg());
}

// ---------- Correção ----------

// Bias constante b com a placa parada e nivelada: erro estacionário = b·τ
static void test_bias_steady_state_error() {
    AttitudeFilter f(P);
    f.update(restForce(0, 0), {0, 0, 0}, 0, NO_SAT);
    for (int i = 0; i < 4000; i++) f.update(restForce(0, 0), {0.5f, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f * P.tauS, f.rollDeg());
}

// Erro inicial sem giro: e(n) = e(0)·αⁿ, α = τ/(τ + dt). Em 3τ resta ~5 %.
static void test_initial_error_decay() {
    AttitudeFilter f(P);
    f.initFrom(10, -6);
    const float alpha = P.tauS / (P.tauS + DT);
    for (int n = 1; n <= 600; n++) {
        f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
        if (n % 100 == 0) {
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f * powf(alpha, n), f.rollDeg());
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, -6.0f * powf(alpha, n), f.pitchDeg());
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, f.rollDeg());   // 3τ: ~5 % de 10°
}

// Ruído típico da bancada (2 mg, 0,035 °/s): o filtro atenua o ruído do ângulo do acelerômetro
static void test_noise_attenuated() {
    AttitudeFilter f(P);
    const Vec3 f0 = restForce(25, -10);
    double sF = 0, qF = 0, sA = 0, qA = 0;
    int n = 0;
    for (int i = 0; i < 3000; i++) {
        const Vec3 a = {f0.x + gauss(0.002f), f0.y + gauss(0.002f), f0.z + gauss(0.002f)};
        f.update(a, {gauss(0.035f), gauss(0.035f), gauss(0.035f)}, i == 0 ? 0 : DT, NO_SAT);
        if (i < 1000) continue;   // depois de 5τ
        const float ra = accelRollPitch(a).rollDeg;
        sF += f.rollDeg(); qF += f.rollDeg() * f.rollDeg();
        sA += ra; qA += ra * ra;
        n++;
    }
    const double mF = sF / n, sdF = sqrt(qF / n - mF * mF), mA = sA / n, sdA = sqrt(qA / n - mA * mA);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.0f, (float)mF);
    TEST_ASSERT_TRUE(sdA > 0.05);
    TEST_ASSERT_TRUE(sdF < 0.2 * sdA);
}

// ---------- Peso e saturação ----------

static void test_acc_weight_limits() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, accWeight(1.00f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, accWeight(1.05f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, accWeight(1.10f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, accWeight(0.90f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, accWeight(1.15f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, accWeight(1.20f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, accWeight(0.80f, false, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, accWeight(1.00f, true, P));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, accWeight(NAN, false, P));
}

// Acelerômetro saturado: a correção é suspensa, a predição continua
static void test_accel_saturation_suspends_correction() {
    AttitudeFilter f(P);
    f.initFrom(5, 0);
    for (int i = 0; i < 200; i++) f.update(restForce(0, 0), {2, 0, 0}, DT, {true, false});
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.0f, f.rollDeg());   // 5° + 2 °/s · 1 s, sem correção
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.lastAccWeight());
    TEST_ASSERT_FALSE(f.degraded());
}

// Giroscópio saturado: estado degradado até acumular 3τ de correção (∑ peso·dt)
static void test_gyro_saturation_degraded() {
    AttitudeFilter f(P);
    f.update(restForce(0, 0), {0, 0, 0}, 0, NO_SAT);
    f.update(restForce(0, 0), {500, 0, 0}, DT, {false, true});
    TEST_ASSERT_TRUE(f.degraded());
    // Sem correção (peso zero), continua degradado
    for (int i = 0; i < 1000; i++) f.update({0, 0, -1.3f}, {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_TRUE(f.degraded());
    // Com peso pleno: 3 s = 600 amostras
    for (int i = 0; i < 590; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_TRUE(f.degraded());
    for (int i = 0; i < 20; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FALSE(f.degraded());
}

// Perto da vertical o peso é pleno, mas o roll não tem referência: essa correção não tira o filtro
// do estado degradado (revisão de 13/10/2026: saía em ~3 s com o roll ainda sem correção)
static void test_degraded_needs_roll_observable() {
    AttitudeFilter f(P);
    f.update(restForce(0, 0), {0, 0, 0}, 0, NO_SAT);
    f.update(restForce(0, 0), {500, 0, 0}, DT, {false, true});
    TEST_ASSERT_TRUE(f.degraded());
    // Pitch 80°: √(ay² + az²) = cos 80° ≈ 0,17 g < 0,3 g, |a| = 1 g. 5 s, bem acima de 3τ
    for (int i = 0; i < 1000; i++) f.update(restForce(0, 80), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, f.lastAccWeight());
    TEST_ASSERT_TRUE(f.degraded());
    // De volta ao nível, os 3τ contam a partir daqui
    for (int i = 0; i < 590; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_TRUE(f.degraded());
    for (int i = 0; i < 20; i++) f.update(restForce(0, 0), {0, 0, 0}, DT, NO_SAT);
    TEST_ASSERT_FALSE(f.degraded());
}

// ---------- Aceleração linear ----------

// Placa nivelada acelerando para a frente por 0,5 s: f = (a, 0, −1) nos eixos do modelo
static float pitchAfterForwardAccel(const FusionParams &p, float accelG) {
    AttitudeFilter f(p);
    f.update(restForce(0, 0), {0, 0, 0}, 0, NO_SAT);
    for (int i = 0; i < 100; i++) f.update({accelG, 0, -1}, {0, 0, 0}, DT, NO_SAT);
    return f.pitchDeg();
}

static void test_horizontal_acceleration() {
    // 0,1 g: |a| = 1,005 g, peso pleno. Passa pelo teste de norma: ~2,2° dos 5,7° aparentes em 0,5 s
    const float apparent = atan2f(0.1f, 1.0f) / D2R;
    const float k = DT / (P.tauS + DT);
    const float expected = apparent * (1 - powf(1 - k, 100));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, expected, pitchAfterForwardAccel(P, 0.1f));
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 2.24f, pitchAfterForwardAccel(P, 0.1f));
    // 0,6 g: |a| = 1,166 g, peso zero: nada entra
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, pitchAfterForwardAccel(P, 0.6f));
    // 0,5 g: peso ~0,32. Comparado com peso fixo (limiares largos), o erro cai a menos da metade
    FusionParams fixedW = P;
    fixedW.accFullDevG = 10;
    fixedW.accZeroDevG = 20;
    const float adaptive = pitchAfterForwardAccel(P, 0.5f), fixed = pitchAfterForwardAccel(fixedW, 0.5f);
    TEST_ASSERT_TRUE(fixed > 10.0f);
    TEST_ASSERT_TRUE(adaptive < 0.5f * fixed);
}

// ---------- Tempo ----------

// Só giroscópio a 10 °/s com dt irregular e lacunas de amostras: depende só do tempo decorrido
static void test_variable_dt_and_gaps() {
    AttitudeFilter g(P, false);
    g.initFrom(0, 0);
    double total = 0;
    for (int i = 0; i < 1000; i++) {
        float dt = 0.0048f + 0.0004f * uniform01();
        if (i % 37 == 0) dt = 0.010f;   // amostra descartada
        if (i % 211 == 0) dt = 0.015f;
        g.update({0, 0, -1}, {10, 0, 0}, dt, NO_SAT);
        total += dt;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)(10.0 * total), g.rollDeg());
}

static void test_dt_limits() {
    AttitudeFilter f(P);
    f.initFrom(5, 0);
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Skipped, (int)f.update(restForce(0, 0), {100, 0, 0}, 0, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Skipped,
                          (int)f.update(restForce(0, 0), {100, 0, 0}, -0.001f, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, f.rollDeg());
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Updated, (int)f.update(restForce(0, 0), {0, 0, 0}, 0.050f, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Reset, (int)f.update(restForce(0, 0), {0, 0, 0}, 0.051f, NO_SAT));
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Initialized,
                          (int)f.update(restForce(-12, 4), {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -12.0f, f.rollDeg());
}

// A virada do micros() é tratada na função que calcula o intervalo: o filtro recebe dt
static void test_elapsed_us_wraparound() {
    TEST_ASSERT_EQUAL_UINT32(5000u, elapsedUs(1000u, 6000u));
    TEST_ASSERT_EQUAL_UINT32(0x200u, elapsedUs(0xFFFFFF00u, 0x00000100u));
    TEST_ASSERT_EQUAL_UINT32(5000u, elapsedUs(0xFFFFF000u, 0xFFFFF000u + 5000u));
}

// ---------- Limites geométricos ----------

// Filtro em 179°, acelerômetro em −179°: corrige pelo caminho curto (passando por 180°, não por 0°)
static void test_roll_wraps_short_path() {
    AttitudeFilter f(P);
    f.initFrom(179, 0);
    for (int i = 0; i < 2000; i++) {
        f.update(restForce(-179, 0), {0, 0, 0}, DT, NO_SAT);
        TEST_ASSERT_TRUE(fabsf(f.rollDeg()) > 178.9f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -179.0f, f.rollDeg());
}

// Rotação em pitch passando pela vertical: sem valores não finitos, pitch dentro de ±90°
static void test_pitch_near_vertical_finite() {
    AttitudeFilter f(P);
    f.update(restForce(10, 60), {0, 0, 0}, 0, NO_SAT);
    TEST_ASSERT_TRUE(f.initialized());
    for (int i = 0; i < 200; i++) {
        const float truth = 60.0f + 60.0f * i * DT;   // até 120° de rotação em torno de y
        const float t = truth * D2R;
        f.update({sinf(t), 0, -cosf(t)}, {0, 60, 0}, DT, NO_SAT);
        TEST_ASSERT_TRUE(isfinite(f.rollDeg()) && isfinite(f.pitchDeg()));
        TEST_ASSERT_TRUE(fabsf(f.pitchDeg()) <= 90.0f);
    }
}

// NaN, infinito e vetor nulo não contaminam o estado
static void test_invalid_data_ignored() {
    AttitudeFilter f(P);
    f.initFrom(3, -2);
    const AttitudeFilter::Step B = AttitudeFilter::Step::BadData;
    TEST_ASSERT_EQUAL_INT((int)B, (int)f.update({NAN, 0, -1}, {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)B, (int)f.update({0, 0, -1}, {INFINITY, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_EQUAL_INT((int)B, (int)f.update({0, 0, -1}, {0, 0, 0}, NAN, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, f.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -2.0f, f.pitchDeg());
    // Vetor nulo: peso zero, só a predição
    TEST_ASSERT_EQUAL_INT((int)AttitudeFilter::Step::Updated, (int)f.update({0, 0, 0}, {0, 0, 0}, DT, NO_SAT));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, f.rollDeg());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -2.0f, f.pitchDeg());
}

// ---------- Calibração de montagem ----------

// Gera as duas medidas: caixa na mesa inclinada (tableRoll, tablePitch) e girada 180° no plano
// da mesa; mount leva vetores dos eixos da caixa para os eixos nominais do sensor
static void twoPoses(const Mat3 &mount, float tableRoll, float tablePitch, Vec3 &f1, Vec3 &f2) {
    const Vec3 b1 = restForce(tableRoll, tablePitch);
    const Vec3 b2 = {-b1.x, -b1.y, b1.z};
    f1 = mat3Apply(mount, b1);
    f2 = mat3Apply(mount, b2);
}

static void checkMount(const Mat3 &mount, float tableRoll, float tablePitch) {
    Vec3 f1, f2;
    twoPoses(mount, tableRoll, tablePitch, f1, f2);
    MountResult r;
    TEST_ASSERT_EQUAL_INT((int)MountCheck::Ok, (int)mountFromTwoPoses(f1, f2, 15.0f, r));
    TEST_ASSERT_TRUE(mat3IsRotation(r.r, 1e-4f));
    // O eixo vertical da caixa é recuperado exatamente, qualquer que seja a mesa
    const Vec3 zb = mat3Apply(r.r, mat3Apply(mount, {0, 0, 1}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, zb.z);
    const Angles m = accelRollPitch(mat3Apply(mount, {0, 0, -1}));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, m.rollDeg, r.mountRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, m.pitchDeg, r.mountPitchDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, tableRoll, r.tableRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, tablePitch, r.tablePitchDeg);
    // Depois da correção, a caixa nivelada lê zero e inclinada lê o ângulo verdadeiro
    const float poses[][2] = {{0, 0}, {30, 0}, {0, -20}, {-25, 15}};
    for (const auto &p : poses) {
        const Angles a = accelRollPitch(mat3Apply(r.r, mat3Apply(mount, restForce(p[0], p[1]))));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, p[0], a.rollDeg);
        TEST_ASSERT_FLOAT_WITHIN(0.05f, p[1], a.pitchDeg);
    }
}

// Montagem 5 (hardware.md): ~2,4° em roll, pitch desprezível; mesa ~0,7° em pitch
static void test_mount_bench_like() {
    checkMount(mul(rotX(2.4f), rotY(-0.3f)), 0.0f, -0.7f);
}

// Mesa muito inclinada (10° e 5°): a soma dos vetores continua dando o eixo da caixa exatamente
static void test_mount_steep_table() {
    checkMount(mul(rotX(-3.0f), rotY(2.0f)), 10.0f, 5.0f);
}

// O nariz é o nominal projetado no plano perpendicular ao eixo z da caixa
static void test_mount_preserves_nose() {
    Vec3 f1, f2;
    twoPoses(mul(rotZ(0), mul(rotX(4.0f), rotY(3.0f))), 2.0f, -1.0f, f1, f2);
    MountResult r;
    TEST_ASSERT_EQUAL_INT((int)MountCheck::Ok, (int)mountFromTwoPoses(f1, f2, 15.0f, r));
    const float zx = r.r.m[2][0];
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, sqrtf(1 - zx * zx), r.r.m[0][0]);   // máximo alinhamento com (1, 0, 0)
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.r.m[0][0] * zx + r.r.m[0][1] * r.r.m[2][1] + r.r.m[0][2] * r.r.m[2][2]);
}

static void test_mount_rejects() {
    MountResult r;
    Vec3 f1, f2;
    twoPoses(rotX(12.0f), 0, 0, f1, f2);
    TEST_ASSERT_EQUAL_INT((int)MountCheck::TooTilted, (int)mountFromTwoPoses(f1, f2, 10.0f, r));
    twoPoses(mat3Identity(), 0, 12.0f, f1, f2);
    TEST_ASSERT_EQUAL_INT((int)MountCheck::TooTilted, (int)mountFromTwoPoses(f1, f2, 10.0f, r));
    TEST_ASSERT_EQUAL_INT((int)MountCheck::BadInput, (int)mountFromTwoPoses({0, 0, -1}, {0, 0, 1}, 10.0f, r));
    TEST_ASSERT_EQUAL_INT((int)MountCheck::BadInput, (int)mountFromTwoPoses({NAN, 0, -1}, {0, 0, -1}, 10.0f, r));
    TEST_ASSERT_EQUAL_INT((int)MountCheck::BadInput, (int)mountFromTwoPoses({0, 0, -0.1f}, {0, 0, -1}, 10.0f, r));
}

static void test_mat3_is_rotation() {
    TEST_ASSERT_TRUE(mat3IsRotation(mat3Identity(), 1e-6f));
    TEST_ASSERT_TRUE(mat3IsRotation(mul(rotX(30), rotY(-20)), 1e-5f));
    const Mat3 reflect = {{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    TEST_ASSERT_FALSE(mat3IsRotation(reflect, 1e-3f));
    const Mat3 scaled = {{{1.01f, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    TEST_ASSERT_FALSE(mat3IsRotation(scaled, 1e-3f));
    const Mat3 bad = {{{NAN, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    TEST_ASSERT_FALSE(mat3IsRotation(bad, 1e-3f));
}

static void test_mat3_rotation_angle() {
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, mat3RotationAngleDeg(mat3Identity()));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, mat3RotationAngleDeg(rotX(30)));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, mat3RotationAngleDeg(rotY(-90)));
    // Montagem da bancada (roll −3,28°, pitch +2,52°): ~4,1° no total
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 4.13f, mat3RotationAngleDeg(mul(rotX(-3.28f), rotY(2.52f))));
    const Mat3 bad = {{{NAN, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    TEST_ASSERT_TRUE(isnan(mat3RotationAngleDeg(bad)));
}

// Repouso da calibração de montagem: rotação lenta constante passa pelo desvio, não pela média
static void test_level_window_rejects_slow_rotation() {
    const RestLimits lim = {0.15f, 0.95f, 1.05f};
    const Vec3 bias = {-4.88f, 2.67f, 1.00f};
    WindowStats g, a;
    for (int i = 0; i < 400; i++) {
        g.add({bias.x + gauss(0.035f), bias.y + gauss(0.035f), bias.z + gauss(0.035f)});
        a.add({gauss(0.002f), gauss(0.002f), 1.0f + gauss(0.002f)});
    }
    TEST_ASSERT_EQUAL_INT((int)LevelRest::Ok, (int)checkLevelWindow(g, bias, a, 360, lim, 0.2f));
    WindowStats gr;
    for (int i = 0; i < 400; i++) gr.add({bias.x + 0.5f + gauss(0.035f), bias.y + gauss(0.035f), bias.z + gauss(0.035f)});
    TEST_ASSERT_TRUE(gr.maxStddev() < 0.15f);
    TEST_ASSERT_EQUAL_INT((int)LevelRest::Rotating, (int)checkLevelWindow(gr, bias, a, 360, lim, 0.2f));
    WindowStats gn = g;
    gn.add({NAN, bias.y, bias.z});
    TEST_ASSERT_EQUAL_INT((int)LevelRest::Invalid, (int)checkLevelWindow(gn, bias, a, 360, lim, 0.2f));
    WindowStats few;
    few.add(bias);
    TEST_ASSERT_EQUAL_INT((int)LevelRest::TooFewSamples, (int)checkLevelWindow(few, bias, a, 360, lim, 0.2f));
}

// Estatística de ângulo do resumo r: sem o salto de ±180°, igual à aritmética longe dele
static void test_angle_stats() {
    AngleStats s;
    TEST_ASSERT_EQUAL_UINT32(0u, s.count());
    s.add(179.9f);
    s.add(-179.9f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, fabsf(s.mean()));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.1f, s.stddev());
    AngleStats t;
    const float v[] = {10.0f, 12.0f, 14.0f};
    for (float x : v) t.add(x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 12.0f, t.mean());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, sqrtf(8.0f / 3.0f), t.stddev());
    AngleStats u;
    u.add(-170.0f);
    u.add(170.0f);
    u.add(-178.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -179.33333f, u.mean());   // média de 190, 170 e 182 = 180,67 → −179,33
    t.reset();
    TEST_ASSERT_EQUAL_UINT32(0u, t.count());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, t.mean());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_chip_to_body_mapping);
    RUN_TEST(test_accel_angles_exact);
    RUN_TEST(test_init_direct_at_rest);
    RUN_TEST(test_init_waits_without_reference);
    RUN_TEST(test_init_averages_noise);
    RUN_TEST(test_init_restarts_on_bad_sample);
    RUN_TEST(test_init_restarts_after_gap);
    RUN_TEST(test_init_bad_data_and_zero_dt);
    RUN_TEST(test_constant_rate_roll);
    RUN_TEST(test_constant_rate_pitch_with_roll);
    RUN_TEST(test_yaw_with_constant_tilt);
    RUN_TEST(test_bias_steady_state_error);
    RUN_TEST(test_initial_error_decay);
    RUN_TEST(test_noise_attenuated);
    RUN_TEST(test_acc_weight_limits);
    RUN_TEST(test_accel_saturation_suspends_correction);
    RUN_TEST(test_gyro_saturation_degraded);
    RUN_TEST(test_degraded_needs_roll_observable);
    RUN_TEST(test_horizontal_acceleration);
    RUN_TEST(test_variable_dt_and_gaps);
    RUN_TEST(test_dt_limits);
    RUN_TEST(test_elapsed_us_wraparound);
    RUN_TEST(test_roll_wraps_short_path);
    RUN_TEST(test_pitch_near_vertical_finite);
    RUN_TEST(test_invalid_data_ignored);
    RUN_TEST(test_mount_bench_like);
    RUN_TEST(test_mount_steep_table);
    RUN_TEST(test_mount_preserves_nose);
    RUN_TEST(test_mount_rejects);
    RUN_TEST(test_mat3_is_rotation);
    RUN_TEST(test_mat3_rotation_angle);
    RUN_TEST(test_level_window_rejects_slow_rotation);
    RUN_TEST(test_angle_stats);
    return UNITY_END();
}
