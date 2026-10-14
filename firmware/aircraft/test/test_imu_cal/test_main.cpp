// Testes da calibração em lógica pura (lib/imu_cal). Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "imu_cal.h"

void setUp() {}
void tearDown() {}

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

static const PoseLimits POSE_LIM = {0.8f, 1.2f};
static const RestLimits REST_LIM = {0.15f, 0.95f, 1.05f};   // mesmos valores do config.h

// Sensor sintético: bruto = offset + escala·g, com g o vetor gravidade (em g) nos eixos do chip
static Vec3 synth(const AccelCal &truth, const Vec3 &g) {
    return {truth.offset.x + truth.scale.x * g.x,
            truth.offset.y + truth.scale.y * g.y,
            truth.offset.z + truth.scale.z * g.z};
}

// Gravidade "para cima" de cada posição, opcionalmente inclinada de tiltDeg num eixo lateral
static Vec3 poseGravity(Pose p, float tiltDeg) {
    const float c = cosf(tiltDeg * (float)M_PI / 180), s = sinf(tiltDeg * (float)M_PI / 180);
    switch (p) {
    case Pose::ZUp:   return {s, 0, c};
    case Pose::ZDown: return {0, s, -c};
    case Pose::XUp:   return {c, 0, s};
    case Pose::XDown: return {-c, s, 0};
    case Pose::YUp:   return {0, c, s};
    default:          return {s, -c, 0};   // YDown
    }
}

// Coeficientes da montagem 5 (docs/hardware.md), em g
static const AccelCal TRUTH = {{0.016479f, 0.000824f, 0.090698f}, {1.005127f, 1.001312f, 1.008545f}};

static void test_window_stats_known_sequence() {
    WindowStats w;
    for (int i = 1; i <= 5; i++) w.add({(float)i, (float)(2 * i), 7.0f});
    TEST_ASSERT_EQUAL_UINT32(5, w.count());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, w.mean().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, w.mean().y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, sqrtf(2.0f), w.stddev().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2 * sqrtf(2.0f), w.stddev().y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, w.stddev().z);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2 * sqrtf(2.0f), w.maxStddev());
    w.reset();
    TEST_ASSERT_EQUAL_UINT32(0, w.count());
}

static void test_six_position_recovers_synthetic_coefficients() {
    Vec3 means[POSE_COUNT];
    for (int i = 0; i < POSE_COUNT; i++) means[i] = synth(TRUTH, poseGravity((Pose)i, 0));
    AccelCal cal;
    TEST_ASSERT_TRUE(sixPositionCal(means, cal));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.offset.x, cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.offset.y, cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.offset.z, cal.offset.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.scale.x, cal.scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.scale.y, cal.scale.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUTH.scale.z, cal.scale.z);
    // Corrigido, |a| = 1 g em todas as posições
    for (int i = 0; i < POSE_COUNT; i++)
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, norm(applyAccelCal(cal, means[i])));
}

// Com cada posição inclinada 3°, o erro dos offsets fica abaixo de 2 mg (como na bancada)
static void test_six_position_tolerates_small_tilt() {
    Vec3 means[POSE_COUNT];
    for (int i = 0; i < POSE_COUNT; i++) means[i] = synth(TRUTH, poseGravity((Pose)i, 3.0f));
    AccelCal cal;
    TEST_ASSERT_TRUE(sixPositionCal(means, cal));
    TEST_ASSERT_FLOAT_WITHIN(0.002f, TRUTH.offset.x, cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, TRUTH.offset.y, cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, TRUTH.offset.z, cal.offset.z);
    TEST_ASSERT_FLOAT_WITHIN(0.003f, TRUTH.scale.z, cal.scale.z);
}

// Médias reais da montagem 5 (LSB em ±2 g, docs/hardware.md) convertidas para g
static void test_six_position_bench_data_montagem5() {
    const float k = 1.0f / 16384;
    Vec3 means[POSE_COUNT];
    means[(int)Pose::ZUp] = {70 * k, 615 * k, 18010 * k};
    means[(int)Pose::ZDown] = {63 * k, -766 * k, -15038 * k};
    means[(int)Pose::XUp] = {16738 * k, 217 * k, 1745 * k};
    means[(int)Pose::XDown] = {-16198 * k, -677 * k, 1468 * k};
    means[(int)Pose::YUp] = {-111 * k, 16419 * k, 910 * k};
    means[(int)Pose::YDown] = {580 * k, -16392 * k, 1769 * k};
    for (int i = 0; i < POSE_COUNT; i++)
        TEST_ASSERT_EQUAL_INT((int)PoseCheck::Ok, (int)checkPose((Pose)i, means[i], POSE_LIM));
    AccelCal cal;
    TEST_ASSERT_TRUE(sixPositionCal(means, cal));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.offset.x, cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.offset.y, cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.offset.z, cal.offset.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.scale.x, cal.scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.scale.y, cal.scale.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, TRUTH.scale.z, cal.scale.z);
    // Critério de validação do calibracao.md: |a| corrigido entre 0,98 e 1,02 g
    float norms[POSE_COUNT];
    TEST_ASSERT_TRUE(validateAccelCal(cal, means, 0.98f, 1.02f, norms));
}

// Posições inclinadas 20°: passam pela verificação de posição e pela faixa de escala, mas a escala
// sai ~6 % menor e |a| corrigido fica em ~1,064 g. A validação precisa reprovar e impedir a gravação.
static void test_validation_rejects_large_tilt() {
    Vec3 means[POSE_COUNT];
    for (int i = 0; i < POSE_COUNT; i++) {
        means[i] = synth(TRUTH, poseGravity((Pose)i, 20.0f));
        TEST_ASSERT_EQUAL_INT((int)PoseCheck::Ok, (int)checkPose((Pose)i, means[i], POSE_LIM));
    }
    AccelCal cal;
    TEST_ASSERT_TRUE(sixPositionCal(means, cal));
    float norms[POSE_COUNT];
    TEST_ASSERT_FALSE(validateAccelCal(cal, means, 0.98f, 1.02f, norms));
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.064f, norms[(int)Pose::ZUp]);
}

static void test_validation_accepts_small_tilt() {
    Vec3 means[POSE_COUNT];
    for (int i = 0; i < POSE_COUNT; i++) means[i] = synth(TRUTH, poseGravity((Pose)i, 3.0f));
    AccelCal cal;
    TEST_ASSERT_TRUE(sixPositionCal(means, cal));
    float norms[POSE_COUNT];
    TEST_ASSERT_TRUE(validateAccelCal(cal, means, 0.98f, 1.02f, norms));
}

static void test_check_pose_rejects_wrong_and_weak() {
    TEST_ASSERT_EQUAL_INT((int)PoseCheck::Ok, (int)checkPose(Pose::ZUp, {0.02f, 0.03f, 1.10f}, POSE_LIM));
    // Outra posição no lugar da pedida: face para baixo em vez de face para cima
    TEST_ASSERT_EQUAL_INT((int)PoseCheck::WrongAxis, (int)checkPose(Pose::ZUp, {0, 0, -0.92f}, POSE_LIM));
    // Nariz para cima quando se pede face para cima
    TEST_ASSERT_EQUAL_INT((int)PoseCheck::WrongAxis, (int)checkPose(Pose::ZUp, {1.0f, 0, 0.05f}, POSE_LIM));
    // Eixo certo, mas inclinado demais (~45°)
    TEST_ASSERT_EQUAL_INT((int)PoseCheck::WeakAxis, (int)checkPose(Pose::XUp, {0.72f, 0, 0.70f}, POSE_LIM));
}

static void test_six_position_rejects_swapped_positions() {
    Vec3 means[POSE_COUNT];
    for (int i = 0; i < POSE_COUNT; i++) means[i] = synth(TRUTH, poseGravity((Pose)i, 0));
    const Vec3 t = means[(int)Pose::XUp];   // as duas posições do eixo X trocadas: escala negativa
    means[(int)Pose::XUp] = means[(int)Pose::XDown];
    means[(int)Pose::XDown] = t;
    AccelCal cal;
    TEST_ASSERT_FALSE(sixPositionCal(means, cal));
}

// Janela de 2 s a 200 Hz com bias e ruído típicos da bancada (docs/hardware.md).
// noiseDps: 0,035 parado; ~0,09 com vibração da mesa (digitação). biasX desloca a média de gx.
static void fillGyroWindow(WindowStats &g, WindowStats &a, float sineAmpDps, float accZ, int n,
                           float noiseDps = 0.035f, float biasX = -4.88f) {
    const Vec3 bias = {biasX, 2.67f, 1.00f};
    g.reset();
    a.reset();
    for (int i = 0; i < n; i++) {
        const float m = sineAmpDps * sinf(2 * (float)M_PI * i / 100.0f);   // 2 Hz
        g.add({bias.x + gauss(noiseDps) + m, bias.y + gauss(noiseDps), bias.z + gauss(noiseDps)});
        a.add({gauss(0.002f), gauss(0.002f), accZ + gauss(0.002f)});
    }
}

static void test_gyro_bias_accepted_at_rest() {
    WindowStats g, a;
    fillGyroWindow(g, a, 0, 1.0f, 400);
    const GyroBiasResult r = evaluateGyroWindow(g, a, 360, REST_LIM);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Ok, (int)r.status);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -4.88f, r.bias.x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.67f, r.bias.y);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.00f, r.bias.z);
    const Vec3 c = applyGyroBias({-4.88f, 2.67f, 1.00f}, r.bias);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.x);
}

static void test_gyro_bias_rejected_when_moving() {
    WindowStats g, a;
    fillGyroWindow(g, a, 5.0f, 1.0f, 400);   // oscilação de 5 °/s: desvio ≈ 3,5 °/s
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Moving, (int)evaluateGyroWindow(g, a, 360, REST_LIM).status);
}

static const float MAX_DELTA = 0.03f;   // mesmos valores do config.h
static const float MAX_TILT = 0.1f;

// Vibração da mesa (bancada, 11/10/2026: desvio 0,08–0,09 °/s com bias correto): passa no filtro
// de desvio e, como a média não se desloca, as duas janelas confirmam o bias
static void test_bias_confirmed_with_vibration() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.09f);
    const GyroBiasResult w1 = evaluateGyroWindow(g, a, 360, REST_LIM);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Ok, (int)w1.status);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Candidate, (int)bc.feed(w1, MAX_DELTA, MAX_TILT));
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.09f);
    const GyroBiasResult w2 = evaluateGyroWindow(g, a, 360, REST_LIM);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Accepted, (int)bc.feed(w2, MAX_DELTA, MAX_TILT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -4.88f, bc.bias().x);
    // O bias aceito é a média das duas janelas
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (w1.bias.x + w2.bias.x) / 2, bc.bias().x);
}

// Caixa se acomodando (bancada, 11/10/2026): desvio baixo, mas a média se desloca ~0,08 °/s entre
// janelas. A diferença é recusada; quando a média se estabiliza, o bias é confirmado
static void test_bias_inconsistent_while_settling() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.05f, -4.86f);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Candidate,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.05f, -4.78f);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Inconsistent,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.08f, bc.lastDelta());
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.05f, -4.79f);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Accepted,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -4.785f, bc.bias().x);
}

// Uma janela reprovada no meio descarta a candidata: o bias só é aceito com duas janelas seguidas
static void test_bias_rejected_window_resets_candidate() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillGyroWindow(g, a, 0, 1.0f, 400);
    bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT);
    fillGyroWindow(g, a, 5.0f, 1.0f, 400);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Rejected,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
    fillGyroWindow(g, a, 0, 1.0f, 400);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Candidate,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
}

// Janela de 2 s a 200 Hz com a caixa girando a rateDps constante em torno do eixo X do chip
// (horizontal com a placa nivelada): gx = bias + taxa e a gravidade gira no plano YZ.
// startDeg: inclinação no início da janela
static void fillRotatingWindow(WindowStats &g, WindowStats &a, float rateDps, float startDeg) {
    g.reset();
    a.reset();
    for (int i = 0; i < 400; i++) {
        const float th = (startDeg + rateDps * i / 200.0f) * (float)M_PI / 180.0f;
        g.add({-4.88f + rateDps + gauss(0.035f), 2.67f + gauss(0.035f), 1.00f + gauss(0.035f)});
        a.add({gauss(0.002f), sinf(th) + gauss(0.002f), cosf(th) + gauss(0.002f)});
    }
}

// Caso da revisão de 13/10/2026: rotação constante de 10 °/s passa pelo desvio, pela norma de a e
// pela comparação das médias do giroscópio. A gravidade gira 20° entre as janelas: recusada sempre
static void test_bias_rejects_constant_horizontal_rotation() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillRotatingWindow(g, a, 10.0f, 0);
    const GyroBiasResult w1 = evaluateGyroWindow(g, a, 360, REST_LIM);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Ok, (int)w1.status);   // os critérios da janela passam
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Candidate, (int)bc.feed(w1, MAX_DELTA, MAX_TILT));
    for (int k = 1; k <= 3; k++) {
        fillRotatingWindow(g, a, 10.0f, 20.0f * k);
        TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Inconsistent,
                              (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
        TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, bc.lastTilt());
        TEST_ASSERT_TRUE(bc.lastDelta() < MAX_DELTA);   // só a gravidade denuncia
    }
}

// Rotação lenta: 0,1 °/s inclina 0,2° entre as janelas (recusada); 0,02 °/s inclina 0,04° (passa,
// e o erro de bias fica abaixo do próprio limite de 0,03 °/s entre janelas)
static void test_bias_tilt_threshold() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillRotatingWindow(g, a, 0.1f, 0);
    bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT);
    fillRotatingWindow(g, a, 0.1f, 0.2f);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Inconsistent,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
    BiasConfirmer slow;
    fillRotatingWindow(g, a, 0.02f, 0);
    slow.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT);
    fillRotatingWindow(g, a, 0.02f, 0.04f);
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Accepted,
                          (int)slow.feed(evaluateGyroWindow(g, a, 360, REST_LIM), MAX_DELTA, MAX_TILT));
}

// Limitação documentada: rotação constante em torno do eixo vertical (Z do chip com a placa
// nivelada) não muda a gravidade e é aceita como bias. Só uma referência externa (magnetômetro)
// distingue esse caso
static void test_bias_vertical_rotation_not_detectable() {
    WindowStats g, a;
    BiasConfirmer bc;
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.035f);
    GyroBiasResult w = evaluateGyroWindow(g, a, 360, REST_LIM);
    w.bias.z += 10.0f;   // gz = bias + 10 °/s, gravidade parada
    bc.feed(w, MAX_DELTA, MAX_TILT);
    fillGyroWindow(g, a, 0, 1.0f, 400, 0.035f);
    w = evaluateGyroWindow(g, a, 360, REST_LIM);
    w.bias.z += 10.0f;
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Accepted, (int)bc.feed(w, MAX_DELTA, MAX_TILT));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.0f, bc.bias().z);
}

// Revisão de 13/10/2026: um NaN em gx some no desvio máximo (fmaxf descarta o NaN) e na diferença
// entre janelas; a janela era aceita e o bias confirmado como NaN
static void test_gyro_bias_rejects_non_finite() {
    WindowStats g, a;
    fillGyroWindow(g, a, 0, 1.0f, 400);
    g.add({NAN, 2.67f, 1.00f});
    TEST_ASSERT_TRUE(g.maxStddev() < 0.15f);   // o desvio máximo sozinho não denuncia
    const GyroBiasResult w = evaluateGyroWindow(g, a, 360, REST_LIM);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Invalid, (int)w.status);
    BiasConfirmer bc;
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Rejected, (int)bc.feed(w, MAX_DELTA, MAX_TILT));
    // Acelerômetro com NaN também
    fillGyroWindow(g, a, 0, 1.0f, 400);
    a.add({0, NAN, 1.0f});
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Invalid, (int)evaluateGyroWindow(g, a, 360, REST_LIM).status);
    // Defesa no confirmador: uma janela marcada Ok com bias NaN não vira candidata
    GyroBiasResult forged = evaluateGyroWindow(g, a, 360, REST_LIM);
    forged.status = RestCheck::Ok;
    forged.bias.x = NAN;
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Rejected, (int)bc.feed(forged, MAX_DELTA, MAX_TILT));
}

// Revisão de 13/10/2026: sem a calibração do acelerômetro, a faixa de |a| era 0..1e9 e um
// acelerômetro zerado passava; o ângulo entre dois vetores nulos dava 0 e o bias era confirmado
static void test_gyro_bias_rejects_null_gravity() {
    const RestLimits RAW_LIM = {0.15f, 0.7f, 1.5f};   // mesmos valores do config.h (antes das seis posições)
    WindowStats g, a;
    fillGyroWindow(g, a, 0, 1.10f, 400);   // repouso típico sem calibração: |a| ≈ 1,10 g
    TEST_ASSERT_EQUAL_INT((int)RestCheck::Ok, (int)evaluateGyroWindow(g, a, 360, RAW_LIM).status);
    fillGyroWindow(g, a, 0, 0.0f, 400);
    a.reset();
    for (int i = 0; i < 400; i++) a.add({0, 0, 0});
    TEST_ASSERT_EQUAL_INT((int)RestCheck::AccOutOfRange, (int)evaluateGyroWindow(g, a, 360, RAW_LIM).status);
    // Mesmo com uma faixa que deixasse passar, o vetor nulo não serve de referência de direção
    const RestLimits OPEN = {0.15f, 0.0f, 1e9f};
    BiasConfirmer bc;
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Candidate,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, OPEN), MAX_DELTA, MAX_TILT));
    TEST_ASSERT_EQUAL_INT((int)BiasConfirmer::Result::Inconsistent,
                          (int)bc.feed(evaluateGyroWindow(g, a, 360, OPEN), MAX_DELTA, MAX_TILT));
}

static void test_angle_between() {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, angleBetweenDeg({0, 0, 1}, {0, 0, 2}));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, angleBetweenDeg({1, 0, 0}, {0, 0, 1}));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, angleBetweenDeg({0, 0, 1}, {0, 0, -1}));
    const float r = 0.01f * (float)M_PI / 180.0f;   // 0,01°: acos em float não resolveria
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.01f, angleBetweenDeg({0, 0, 1}, {0, sinf(r), cosf(r)}));
    TEST_ASSERT_TRUE(isnan(angleBetweenDeg({0, 0, 0}, {0, 0, 1})));   // vetor nulo: sem direção
    TEST_ASSERT_TRUE(isnan(angleBetweenDeg({0, 0, 1}, {0, 0, 0})));
    TEST_ASSERT_TRUE(isnan(angleBetweenDeg({NAN, 0, 1}, {0, 0, 1})));
}

static void test_gyro_bias_rejected_when_accelerating() {
    WindowStats g, a;
    fillGyroWindow(g, a, 0, 1.2f, 400);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::AccOutOfRange, (int)evaluateGyroWindow(g, a, 360, REST_LIM).status);
}

static void test_gyro_bias_rejected_when_window_incomplete() {
    WindowStats g, a;
    fillGyroWindow(g, a, 0, 1.0f, 200);
    TEST_ASSERT_EQUAL_INT((int)RestCheck::TooFewSamples, (int)evaluateGyroWindow(g, a, 360, REST_LIM).status);
}

static void test_temp_drift() {
    TEST_ASSERT_FALSE(tempDrifted(26.0f, 30.0f, 10.0f));
    TEST_ASSERT_TRUE(tempDrifted(26.0f, 36.5f, 10.0f));
    TEST_ASSERT_TRUE(tempDrifted(26.0f, 15.5f, 10.0f));
}

// Registro carregado da NVS: só valores finitos e numa faixa possível
static void test_accel_cal_plausible() {
    const AccelCal ok = {{0.03f, -0.02f, 0.07f}, {1.001f, 0.998f, 0.985f}};
    TEST_ASSERT_TRUE(accelCalPlausible(ok, 0.5f));
    AccelCal c = ok;
    c.scale.y = 0;   // escala zero: dividiria por zero
    TEST_ASSERT_FALSE(accelCalPlausible(c, 0.5f));
    c = ok;
    c.scale.z = -1.0f;
    TEST_ASSERT_FALSE(accelCalPlausible(c, 0.5f));
    c = ok;
    c.offset.x = NAN;
    TEST_ASSERT_FALSE(accelCalPlausible(c, 0.5f));
    c = ok;
    c.scale.x = INFINITY;
    TEST_ASSERT_FALSE(accelCalPlausible(c, 0.5f));
    c = ok;
    c.offset.z = -0.6f;
    TEST_ASSERT_FALSE(accelCalPlausible(c, 0.5f));
}

static void test_gyro_bias_plausible() {
    TEST_ASSERT_TRUE(gyroBiasPlausible({-4.88f, 1.2f, 0.4f}, 20.0f));   // bancada: gx ≈ −4,9 °/s
    TEST_ASSERT_FALSE(gyroBiasPlausible({0, NAN, 0}, 20.0f));
    TEST_ASSERT_FALSE(gyroBiasPlausible({0, 0, -INFINITY}, 20.0f));
    TEST_ASSERT_FALSE(gyroBiasPlausible({25.0f, 0, 0}, 20.0f));
}

// A montagem só vale com a calibração do acelerômetro com que foi medida
static void test_same_accel_cal() {
    const AccelCal a = {{0.03f, -0.02f, 0.07f}, {1.001f, 0.998f, 0.985f}};
    AccelCal b = a;
    TEST_ASSERT_TRUE(sameAccelCal(true, a, true, b));
    TEST_ASSERT_TRUE(sameAccelCal(false, a, false, AccelCal()));   // duas ausências
    TEST_ASSERT_FALSE(sameAccelCal(true, a, false, a));            // calibração apagada ou de outra faixa
    TEST_ASSERT_FALSE(sameAccelCal(false, a, true, a));            // calibração feita depois da montagem
    b.offset.y += 0.001f;
    TEST_ASSERT_FALSE(sameAccelCal(true, a, true, b));
    b = a;
    b.scale.z = 0.986f;
    TEST_ASSERT_FALSE(sameAccelCal(true, a, true, b));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_window_stats_known_sequence);
    RUN_TEST(test_six_position_recovers_synthetic_coefficients);
    RUN_TEST(test_six_position_tolerates_small_tilt);
    RUN_TEST(test_six_position_bench_data_montagem5);
    RUN_TEST(test_validation_rejects_large_tilt);
    RUN_TEST(test_validation_accepts_small_tilt);
    RUN_TEST(test_check_pose_rejects_wrong_and_weak);
    RUN_TEST(test_six_position_rejects_swapped_positions);
    RUN_TEST(test_gyro_bias_accepted_at_rest);
    RUN_TEST(test_gyro_bias_rejected_when_moving);
    RUN_TEST(test_bias_confirmed_with_vibration);
    RUN_TEST(test_bias_inconsistent_while_settling);
    RUN_TEST(test_bias_rejected_window_resets_candidate);
    RUN_TEST(test_bias_rejects_constant_horizontal_rotation);
    RUN_TEST(test_bias_tilt_threshold);
    RUN_TEST(test_bias_vertical_rotation_not_detectable);
    RUN_TEST(test_gyro_bias_rejects_non_finite);
    RUN_TEST(test_gyro_bias_rejects_null_gravity);
    RUN_TEST(test_angle_between);
    RUN_TEST(test_gyro_bias_rejected_when_accelerating);
    RUN_TEST(test_gyro_bias_rejected_when_window_incomplete);
    RUN_TEST(test_temp_drift);
    RUN_TEST(test_accel_cal_plausible);
    RUN_TEST(test_gyro_bias_plausible);
    RUN_TEST(test_same_accel_cal);
    return UNITY_END();
}
