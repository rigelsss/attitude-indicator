// Testes dos fluxos da calibração (lib/cal_flow): giroscópio (boot e g) e coordenador.
// Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "cal_flow.h"

void setUp() {}
void tearDown() {}

// Mesmos valores do config.h: janela de 2 s a 200 Hz, mínimo de 90 % (360 amostras)
static const GyroCalParams GP = {360, {0.15f, 0.95f, 1.05f}, {0.15f, 0.7f, 1.5f}, 0.03f, 0.1f};
static const SixPoseParams SP = {360, 0.15f, {0.8f, 1.2f}, 0.98f, 1.02f};
static const MountParams MP = {360, {0.15f, 0.95f, 1.05f}, 0.2f, 10.0f};
static const CalCoordinatorParams CP = {2000000, GP, SP, MP};
static const uint32_t PERIOD_US = 5000;

static const Vec3 BIAS = {0.5f, -0.3f, 0.1f};
static const Vec3 LEVEL_G = {0, 0, 1.0f};

// Alimenta amostras a 200 Hz até uma janela terminar (ou o limite de amostras) e, como o serviço, abre
// a próxima janela pendente depois. moving: o giroscópio alterna ±1 °/s em torno de gyro (desvio de
// 1 °/s, acima do limite de repouso).
static CalCoordinator::Done feedWindow(CalCoordinator &c, uint32_t &t, const Vec3 &gyro, const Vec3 &acc,
                                       bool accelValid = true, bool moving = false, uint32_t maxSamples = 1000,
                                       float tempC = 25.0f) {
    for (uint32_t i = 0; i < maxSamples; i++) {
        t += PERIOD_US;
        Vec3 g = gyro;
        if (moving) g.x += (i % 2) ? 1.0f : -1.0f;
        const CalCoordinator::Done d = c.feed(g, acc, acc, tempC, t, accelValid);
        if (d != CalCoordinator::Done::None) {
            c.startPendingWindow(t);
            return d;
        }
    }
    return CalCoordinator::Done::None;
}

static void feedStillWindow(CalCoordinator &c, uint32_t &t, const Vec3 &gyro, float tempC) {
    feedWindow(c, t, gyro, LEVEL_G, true, false, 1000, tempC);
}

// Duas janelas paradas e iguais: candidata e aceite, bias = média, modo volta a None
static void test_gyro_accepted() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Gyro, feedWindow(c, t, BIAS, LEVEL_G));
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    TEST_ASSERT_TRUE(c.window().active);
    TEST_ASSERT_EQUAL_UINT32(400, c.gyro().lastCount());
    TEST_ASSERT_FALSE(c.gyro().definitive());
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Gyro, feedWindow(c, t, BIAS, LEVEL_G));
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
    TEST_ASSERT_TRUE(c.gyro().definitive());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, BIAS.x, c.gyro().bias().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, BIAS.y, c.gyro().bias().y);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, BIAS.z, c.gyro().bias().z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, c.gyro().biasTempC());
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.window().active);
}

// Janelas diferentes dentro dos limites: bias e temperatura são a média das duas
static void test_gyro_accepted_averages_windows() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedStillWindow(c, t, {0.50f, -0.30f, 0.10f}, 24.0f);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    feedStillWindow(c, t, {0.52f, -0.29f, 0.11f}, 26.0f);   // diferença de 0,02 °/s, limite de 0,03
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.51f, c.gyro().bias().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -0.295f, c.gyro().bias().y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.105f, c.gyro().bias().z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, c.gyro().biasTempC());
}

// Depois de uma inconsistência, a temperatura da candidata é a da janela que virou candidata
static void test_gyro_inconsistent_candidate_temperature() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Cmd, t);
    feedStillWindow(c, t, {0.50f, 0, 0}, 20.0f);
    feedStillWindow(c, t, {0.60f, 0, 0}, 24.0f);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Inconsistent, c.lastGyroEvent());
    feedStillWindow(c, t, {0.62f, 0, 0}, 26.0f);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.61f, c.gyro().bias().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, c.gyro().biasTempC());
}

// A próxima janela não abre sozinha: fica pendente até quem chama imprimir e abri-la
static void test_next_window_pending_until_started() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    CalCoordinator::Done d = CalCoordinator::Done::None;
    while (d == CalCoordinator::Done::None) {
        t += PERIOD_US;
        d = c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, t, true);
    }
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_TRUE(c.windowPending());
    // Amostra durante a impressão: fica fora de qualquer janela
    TEST_ASSERT_EQUAL(CalCoordinator::Done::None, c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, t + 3000000, true));
    t += 50000;   // impressão bloqueada por 50 ms
    c.startPendingWindow(t);
    TEST_ASSERT_TRUE(c.window().active);
    TEST_ASSERT_FALSE(c.windowPending());
    TEST_ASSERT_EQUAL_UINT32(t, c.window().startUs);
    TEST_ASSERT_EQUAL_UINT32(0, c.window().gyro.count());
    // Sem pendência, startPendingWindow não reabre a janela em andamento
    c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, t + PERIOD_US, true);
    c.startPendingWindow(t + 2 * PERIOD_US);
    TEST_ASSERT_EQUAL_UINT32(t, c.window().startUs);
    TEST_ASSERT_EQUAL_UINT32(1, c.window().gyro.count());
    // Aceite: nada pendente
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
    TEST_ASSERT_FALSE(c.windowPending());
}

// Bias deslocado entre as janelas: inconsistente, e a última vira a candidata
static void test_gyro_inconsistent_then_accepted() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Cmd, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    const Vec3 shifted = {BIAS.x + 0.1f, BIAS.y, BIAS.z};
    feedWindow(c, t, shifted, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Inconsistent, c.lastGyroEvent());
    TEST_ASSERT_EQUAL(CalMode::GyroCmd, c.mode());
    feedWindow(c, t, shifted, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, shifted.x, c.gyro().bias().x);
}

// Gravidade inclinada entre as janelas (rotação lenta): também inconsistente
static void test_gyro_inconsistent_by_tilt() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    const float a = 0.5f * (float)M_PI / 180;   // 0,5°, acima do limite de 0,1°
    feedWindow(c, t, BIAS, {sinf(a), 0, cosf(a)});
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Inconsistent, c.lastGyroEvent());
    TEST_ASSERT_FALSE(c.gyro().definitive());
}

// Boot com a caixa mexendo: recusa e nova tentativa, com o bias provisório em uso
static void test_boot_rejected_retries() {
    CalCoordinator c(CP);
    c.gyro().setProvisional(BIAS, 22.0f);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G, true, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedRetry, c.lastGyroEvent());
    TEST_ASSERT_EQUAL(RestCheck::Moving, c.gyro().lastWindow().status);
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_TRUE(c.window().active);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, BIAS.x, c.gyro().bias().x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 22.0f, c.gyro().biasTempC());
    // Parada de novo: candidata e aceite
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Accepted, c.lastGyroEvent());
}

// A recusa descarta a candidata: depois dela, a janela parada volta a ser só candidata
static void test_rejection_discards_candidate() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G, true, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedRetry, c.lastGyroEvent());
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
}

// g recusado com bias deste boot: bias anterior mantido e modo None
static void test_cmd_rejected_keeps_bias() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_TRUE(c.gyro().definitive());
    c.startGyro(GyroCalKind::Cmd, t);
    TEST_ASSERT_EQUAL(CalMode::GyroCmd, c.mode());
    feedWindow(c, t, {2, 2, 2}, LEVEL_G, true, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedKept, c.lastGyroEvent());
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_TRUE(c.gyro().definitive());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, BIAS.x, c.gyro().bias().x);
}

// g recusado sem bias deste boot (g apertado durante a calibração do boot): volta a do boot
static void test_cmd_rejected_without_boot_bias_restarts_boot() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    c.startGyro(GyroCalKind::Cmd, t);
    feedWindow(c, t, BIAS, LEVEL_G, true, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedKeptBootRestart, c.lastGyroEvent());
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_EQUAL(GyroCalKind::Boot, c.gyro().kind());
    TEST_ASSERT_TRUE(c.window().active);
    // A próxima recusa já é a do boot
    feedWindow(c, t, BIAS, LEVEL_G, true, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedRetry, c.lastGyroEvent());
}

// Janela com poucas amostras (falhas de leitura): recusada por janela incompleta
static void test_window_too_few_samples() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    // Amostras a cada 10 ms: 200 na janela, abaixo do mínimo de 360
    CalCoordinator::Done d = CalCoordinator::Done::None;
    while (d == CalCoordinator::Done::None) {
        t += 2 * PERIOD_US;
        d = c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, t, true);
    }
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedRetry, c.lastGyroEvent());
    TEST_ASSERT_EQUAL(RestCheck::TooFewSamples, c.gyro().lastWindow().status);
    TEST_ASSERT_EQUAL_UINT32(200, c.gyro().lastCount());
}

// Sem calibração do acelerômetro, |a| = 1,10 g passa pela faixa larga; com calibração, é recusado
static void test_raw_accel_limits() {
    const Vec3 acc = {0, 0, 1.10f};
    CalCoordinator raw(CP);
    uint32_t t = 0;
    raw.startGyro(GyroCalKind::Boot, t);
    feedWindow(raw, t, BIAS, acc, false);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, raw.lastGyroEvent());

    CalCoordinator cal(CP);
    t = 0;
    cal.startGyro(GyroCalKind::Boot, t);
    feedWindow(cal, t, BIAS, acc, true);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::RejectedRetry, cal.lastGyroEvent());
    TEST_ASSERT_EQUAL(RestCheck::AccOutOfRange, cal.gyro().lastWindow().status);
}

// MPU reconfigurado com uma candidata: a janela recomeça e a próxima janela parada volta a ser candidata
static void test_mpu_reconfigured_restarts_window() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    // Metade da janela seguinte, e então a reconfiguração
    feedWindow(c, t, BIAS, LEVEL_G, true, false, 200);
    t += 50000;
    c.onMpuReconfigured(t);
    TEST_ASSERT_TRUE(c.window().active);
    TEST_ASSERT_EQUAL_UINT32(t, c.window().startUs);
    TEST_ASSERT_EQUAL_UINT32(0, c.window().gyro.count());
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(GyroCalFlow::Event::Candidate, c.lastGyroEvent());
    TEST_ASSERT_EQUAL_UINT32(400, c.gyro().lastCount());
}

// Sem janela ativa, a reconfiguração não abre uma
static void test_mpu_reconfigured_without_window() {
    CalCoordinator c(CP);
    c.onMpuReconfigured(1000);
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_EQUAL(CalCoordinator::Done::None, c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, 5000000, true));
}

// Bloqueios: m liberado na calibração do boot e bloqueado no g e no 6; z com os pré-requisitos, nesta ordem
static void test_blocking_rules() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    TEST_ASSERT_TRUE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::NoAccel, c.checkMount(false));
    TEST_ASSERT_EQUAL(MountStart::GyroProvisional, c.checkMount(true));
    TEST_ASSERT_EQUAL(MountStart::GyroProvisional, c.startMount(true));
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    c.startGyro(GyroCalKind::Boot, t);
    TEST_ASSERT_TRUE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::Busy, c.checkMount(true));
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(MountStart::Ok, c.checkMount(true));
    TEST_ASSERT_EQUAL(MountStart::NoAccel, c.checkMount(false));
    c.startGyro(GyroCalKind::Cmd, t);
    TEST_ASSERT_FALSE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::Busy, c.startMount(true));
    TEST_ASSERT_EQUAL(CalMode::GyroCmd, c.mode());
    c.startSix();
    TEST_ASSERT_FALSE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::Busy, c.checkMount(true));
}

// ---------- Seis posições e montagem ----------

static void calibrateGyro(CalCoordinator &c, uint32_t &t) {
    c.startGyro(GyroCalKind::Boot, t);
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_TRUE(c.gyro().definitive());
}

// Enter na espera e uma janela de medida (como o serviço: a janela abre depois da mensagem)
static CalCoordinator::Done measure(CalCoordinator &c, uint32_t &t, const Vec3 &gyro, const Vec3 &acc,
                                    bool moving = false) {
    TEST_ASSERT_TRUE(c.confirm());
    TEST_ASSERT_FALSE(c.window().active);
    c.startPendingWindow(t);
    return feedWindow(c, t, gyro, acc, true, moving);
}

// Média ideal de cada posição nos eixos do chip (ZUp, ZDown, XUp, XDown, YUp, YDown): o eixo da
// posição com scale·(±1 g) mais offset; tiltDeg inclina a gravidade para o eixo seguinte
static Vec3 poseMean(int pose, const Vec3 &offset, float scale = 1.0f, float tiltDeg = 0) {
    static const int AXIS[POSE_COUNT] = {2, 2, 0, 0, 1, 1};
    const float sign = (pose % 2 == 0) ? 1.0f : -1.0f;
    const float a = tiltDeg * (float)M_PI / 180;
    float v[3] = {0, 0, 0};
    v[AXIS[pose]] = sign * scale * cosf(a);
    v[(AXIS[pose] + 1) % 3] = sinf(a);
    return {v[0] + offset.x, v[1] + offset.y, v[2] + offset.z};
}

static const Vec3 ACC_OFFSET = {0.02f, -0.01f, 0.10f};

// Seis posições boas: cada uma aceita e a próxima pedida; depois da sexta, offset e escala recuperados
static void test_six_ready_to_save() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startSix();
    TEST_ASSERT_EQUAL(CalMode::SixWait, c.mode());
    for (int i = 0; i < POSE_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(i, c.six().index());
        TEST_ASSERT_EQUAL(CalCoordinator::Done::Six, measure(c, t, BIAS, poseMean(i, ACC_OFFSET, 1.01f)));
        if (i < POSE_COUNT - 1) {
            TEST_ASSERT_EQUAL(SixPoseFlow::Event::PoseOk, c.lastSixEvent());
            TEST_ASSERT_EQUAL(CalMode::SixWait, c.mode());
        }
    }
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::ReadyToSave, c.lastSixEvent());
    TEST_ASSERT_EQUAL(CalMode::SixReview, c.mode());
    const AccelCal &r = c.six().result();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, ACC_OFFSET.x, r.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, ACC_OFFSET.y, r.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, ACC_OFFSET.z, r.offset.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.01f, r.scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.01f, r.scale.z);
    for (int i = 0; i < POSE_COUNT; i++) {
        TEST_ASSERT_TRUE(c.six().normOk(i));
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, c.six().poseNorm(i));
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, c.six().tempC());
    TEST_ASSERT_FALSE(c.confirm());   // Enter na revisão não mede de novo
    // s ou x: sem bias deste boot, a calibração do boot recomeça
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_TRUE(c.gyroRestartPending());
}

// Posição recusada (incompleta, caixa mexida, outro eixo, inclinada demais): pedida de novo
static void test_six_rejected_pose_repeats() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startSix();
    // Amostras a cada 10 ms: 200 na janela, abaixo do mínimo
    TEST_ASSERT_TRUE(c.confirm());
    c.startPendingWindow(t);
    CalCoordinator::Done d = CalCoordinator::Done::None;
    while (d == CalCoordinator::Done::None) {
        t += 2 * PERIOD_US;
        d = c.feed(BIAS, LEVEL_G, LEVEL_G, 25.0f, t, true);
    }
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::Incomplete, c.lastSixEvent());
    TEST_ASSERT_EQUAL_UINT32(200, c.six().lastCount());
    TEST_ASSERT_EQUAL(CalMode::SixWait, c.mode());

    measure(c, t, BIAS, LEVEL_G, true);
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::Moved, c.lastSixEvent());
    TEST_ASSERT_TRUE(c.six().lastGyroStd() > 0.9f);

    measure(c, t, BIAS, poseMean((int)Pose::XUp, {0, 0, 0}));   // pedida a ZUp
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::WrongPose, c.lastSixEvent());
    TEST_ASSERT_EQUAL(PoseCheck::WrongAxis, c.six().lastPoseCheck());

    measure(c, t, BIAS, {0, 0, 0.7f});
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::WrongPose, c.lastSixEvent());
    TEST_ASSERT_EQUAL(PoseCheck::WeakAxis, c.six().lastPoseCheck());
    TEST_ASSERT_EQUAL_INT(0, c.six().index());
    TEST_ASSERT_EQUAL(CalMode::SixWait, c.mode());

    measure(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::PoseOk, c.lastSixEvent());
    TEST_ASSERT_EQUAL_INT(1, c.six().index());
}

// Posições inclinadas 20°: passam pela conferência de cada posição, mas a validação reprova
static void test_six_validation_fail() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    calibrateGyro(c, t);
    c.startSix();
    for (int i = 0; i < POSE_COUNT; i++) measure(c, t, BIAS, poseMean(i, {0, 0, 0}, 1.0f, 20.0f));
    TEST_ASSERT_EQUAL(SixPoseFlow::Event::ValidationFail, c.lastSixEvent());
    TEST_ASSERT_FALSE(c.six().normOk(0));
    TEST_ASSERT_TRUE(c.six().poseNorm(0) > 1.02f);
    // Com bias deste boot, volta a None sem recomeçar o giroscópio
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.windowPending());
}

// 6 interrompe o giroscópio; Enter só na espera; x em qualquer etapa
static void test_six_confirm_and_cancel() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    c.startSix();
    TEST_ASSERT_FALSE(c.window().active);   // a janela do giroscópio foi interrompida
    TEST_ASSERT_FALSE(c.windowPending());
    TEST_ASSERT_TRUE(c.confirm());
    TEST_ASSERT_EQUAL(CalMode::SixMeasure, c.mode());
    c.startPendingWindow(t);
    TEST_ASSERT_TRUE(c.window().active);
    TEST_ASSERT_FALSE(c.confirm());   // Enter extra durante a medida é ignorado
    TEST_ASSERT_TRUE(c.window().active);
    // x durante a medida, sem bias deste boot: a calibração do boot recomeça depois da mensagem
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_TRUE(c.gyroRestartPending());
    c.startPendingWindow(t);
    TEST_ASSERT_TRUE(c.window().active);
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_TRUE(c.gyro().definitive());
    // x na espera, com bias deste boot: volta a None
    c.startSix();
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.windowPending());
    // exitFlow fora das seis posições e da montagem não faz nada
    c.startGyro(GyroCalKind::Cmd, t);
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::GyroCmd, c.mode());
    TEST_ASSERT_TRUE(c.window().active);
}

// Acelerômetro nos eixos do chip com a caixa em roll φ e pitch θ (sem desalinhamento). A força
// específica nos eixos da caixa é (sin θ, −sin φ·cos θ, −cos φ·cos θ), e chipToBody(c) = (c.y, c.x, −c.z).
static Vec3 chipTilted(float rollDeg, float pitchDeg) {
    const float r = rollDeg * (float)M_PI / 180, p = pitchDeg * (float)M_PI / 180;
    return {-sinf(r) * cosf(p), sinf(p), cosf(r) * cosf(p)};
}

// Caixa girada 180° no plano da mesa: x e y da caixa trocam de sinal, e no chip também
static Vec3 turned180(const Vec3 &chip) { return {-chip.x, -chip.y, chip.z}; }

// Duas medidas boas: módulo em roll −3° e pitch +2° (igual nas duas) com a mesa nivelada; depois,
// mesa em roll +1,5° e pitch −2° (troca de sinal entre as duas) com o módulo alinhado
static void test_mount_ready_to_save() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    calibrateGyro(c, t);
    TEST_ASSERT_EQUAL(MountStart::Ok, c.startMount(true));
    TEST_ASSERT_EQUAL(CalMode::LevelWait, c.mode());
    TEST_ASSERT_EQUAL_INT(0, c.mount().index());
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Level, measure(c, t, BIAS, chipTilted(-3.0f, 2.0f)));
    TEST_ASSERT_EQUAL(MountFlow::Event::PoseOk, c.lastMountEvent());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -3.0f, c.mount().lastAngles().rollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, c.mount().lastAngles().pitchDeg);
    TEST_ASSERT_EQUAL_INT(1, c.mount().index());
    TEST_ASSERT_EQUAL(CalMode::LevelWait, c.mode());
    measure(c, t, BIAS, chipTilted(-3.0f, 2.0f));
    TEST_ASSERT_EQUAL(MountFlow::Event::ReadyToSave, c.lastMountEvent());
    TEST_ASSERT_EQUAL(CalMode::LevelReview, c.mode());
    const MountResult &r = c.mount().result();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -3.0f, r.mountRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, r.mountPitchDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.tableRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.tablePitchDeg);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, c.mount().tempC());
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.windowPending());

    c.startMount(true);
    const Vec3 table = chipTilted(1.5f, -2.0f);
    measure(c, t, BIAS, table);
    measure(c, t, BIAS, turned180(table));
    TEST_ASSERT_EQUAL(MountFlow::Event::ReadyToSave, c.lastMountEvent());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.mount().result().mountRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.mount().result().mountPitchDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, c.mount().result().tableRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -2.0f, c.mount().result().tablePitchDeg);
}

// Medida recusada (movimento, rotação lenta): pedida de novo; incoerentes e inclinadas: descartadas
static void test_mount_rejections() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    calibrateGyro(c, t);
    c.startMount(true);
    measure(c, t, BIAS, LEVEL_G, true);
    TEST_ASSERT_EQUAL(MountFlow::Event::Rejected, c.lastMountEvent());
    TEST_ASSERT_EQUAL(LevelRest::Moving, c.mount().lastRest());
    TEST_ASSERT_EQUAL(CalMode::LevelWait, c.mode());
    measure(c, t, {BIAS.x + 0.5f, BIAS.y, BIAS.z}, LEVEL_G);   // 0,5 °/s constante: rotação lenta
    TEST_ASSERT_EQUAL(MountFlow::Event::Rejected, c.lastMountEvent());
    TEST_ASSERT_EQUAL(LevelRest::Rotating, c.mount().lastRest());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, c.mount().lastGyroMean().x);
    TEST_ASSERT_EQUAL_INT(0, c.mount().index());

    // A caixa de cabeça para baixo na segunda: as duas medidas não são da mesma face
    measure(c, t, BIAS, LEVEL_G);
    measure(c, t, BIAS, {0, 0, -1.0f});
    TEST_ASSERT_EQUAL(MountFlow::Event::BadInput, c.lastMountEvent());
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());

    c.startMount(true);
    measure(c, t, BIAS, chipTilted(12.0f, 0));
    measure(c, t, BIAS, chipTilted(12.0f, 0));
    TEST_ASSERT_EQUAL(MountFlow::Event::TooTilted, c.lastMountEvent());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.0f, c.mount().result().mountRollDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.mount().result().mountPitchDeg);
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
}

// x na espera, na medida e na revisão: volta a None, sem janela
static void test_mount_cancel() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    calibrateGyro(c, t);
    c.startMount(true);
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    c.startMount(true);
    TEST_ASSERT_TRUE(c.confirm());
    c.startPendingWindow(t);
    TEST_ASSERT_FALSE(c.confirm());
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_FALSE(c.windowPending());
    c.startMount(true);
    measure(c, t, BIAS, LEVEL_G);
    measure(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(CalMode::LevelReview, c.mode());
    TEST_ASSERT_FALSE(c.confirm());
    c.exitFlow();
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.windowPending());
}

// A janela fecha pelo instante da amostra, também na virada do micros(); o descarte de partida adia o início
static void test_window_wraparound_and_hold() {
    CalCoordinator c(CP);
    uint32_t t = 0xFFFFFFFFu - 1000000;   // virada no meio da janela
    c.startGyro(GyroCalKind::Boot, t);
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Gyro, feedWindow(c, t, BIAS, LEVEL_G));
    TEST_ASSERT_EQUAL_UINT32(400, c.gyro().lastCount());

    CalCoordinator h(CP);
    t = 0;
    h.startGyro(GyroCalKind::Boot, t);
    h.holdWindowStart(1000000);   // descarte de partida até 1 s
    t = 1000000;
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Gyro, feedWindow(h, t, BIAS, LEVEL_G));
    TEST_ASSERT_EQUAL_UINT32(400, h.gyro().lastCount());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_gyro_accepted);
    RUN_TEST(test_gyro_accepted_averages_windows);
    RUN_TEST(test_gyro_inconsistent_candidate_temperature);
    RUN_TEST(test_next_window_pending_until_started);
    RUN_TEST(test_gyro_inconsistent_then_accepted);
    RUN_TEST(test_gyro_inconsistent_by_tilt);
    RUN_TEST(test_boot_rejected_retries);
    RUN_TEST(test_rejection_discards_candidate);
    RUN_TEST(test_cmd_rejected_keeps_bias);
    RUN_TEST(test_cmd_rejected_without_boot_bias_restarts_boot);
    RUN_TEST(test_window_too_few_samples);
    RUN_TEST(test_raw_accel_limits);
    RUN_TEST(test_mpu_reconfigured_restarts_window);
    RUN_TEST(test_mpu_reconfigured_without_window);
    RUN_TEST(test_blocking_rules);
    RUN_TEST(test_six_ready_to_save);
    RUN_TEST(test_six_rejected_pose_repeats);
    RUN_TEST(test_six_validation_fail);
    RUN_TEST(test_six_confirm_and_cancel);
    RUN_TEST(test_mount_ready_to_save);
    RUN_TEST(test_mount_rejections);
    RUN_TEST(test_mount_cancel);
    RUN_TEST(test_window_wraparound_and_hold);
    return UNITY_END();
}
