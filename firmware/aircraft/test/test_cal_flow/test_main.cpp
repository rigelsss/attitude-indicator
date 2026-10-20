// Testes dos fluxos da calibração (lib/cal_flow): giroscópio (boot e g) e coordenador.
// Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "cal_flow.h"

void setUp() {}
void tearDown() {}

// Mesmos valores do config.h: janela de 2 s a 200 Hz, mínimo de 90 % (360 amostras)
static const GyroCalParams GP = {360, {0.15f, 0.95f, 1.05f}, {0.15f, 0.7f, 1.5f}, 0.03f, 0.1f};
static const CalCoordinatorParams CP = {2000000, GP};
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

// Bloqueios: m liberado na calibração do boot e bloqueado no g; z com os pré-requisitos, nesta ordem
static void test_blocking_rules() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    TEST_ASSERT_TRUE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::NoAccel, c.checkMount(false));
    TEST_ASSERT_EQUAL(MountStart::GyroProvisional, c.checkMount(true));
    c.startGyro(GyroCalKind::Boot, t);
    TEST_ASSERT_TRUE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::Busy, c.checkMount(true));
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    TEST_ASSERT_EQUAL(MountStart::Ok, c.checkMount(true));
    TEST_ASSERT_EQUAL(MountStart::NoAccel, c.checkMount(false));
    c.startGyro(GyroCalKind::Cmd, t);
    TEST_ASSERT_FALSE(c.magAllowed());
    TEST_ASSERT_EQUAL(MountStart::Busy, c.checkMount(true));
    c.setMode(CalMode::SixWait);
    TEST_ASSERT_FALSE(c.magAllowed());
}

// Fim das seis posições: retoma a calibração do boot só se ainda não há bias deste boot
static void test_end_six() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.startGyro(GyroCalKind::Boot, t);
    c.cancelWindow();
    c.setMode(CalMode::SixReview);
    TEST_ASSERT_TRUE(c.endSix());
    TEST_ASSERT_EQUAL(CalMode::GyroBoot, c.mode());
    TEST_ASSERT_FALSE(c.window().active);   // abre depois da mensagem
    TEST_ASSERT_TRUE(c.windowPending());
    c.startPendingWindow(t);
    TEST_ASSERT_TRUE(c.window().active);
    feedWindow(c, t, BIAS, LEVEL_G);
    feedWindow(c, t, BIAS, LEVEL_G);
    c.setMode(CalMode::SixReview);
    TEST_ASSERT_FALSE(c.endSix());
    TEST_ASSERT_EQUAL(CalMode::None, c.mode());
    TEST_ASSERT_FALSE(c.windowPending());
    c.startPendingWindow(t);
    TEST_ASSERT_FALSE(c.window().active);
}

// Janela das seis posições e da montagem: o fim é devolvido sem passar pelo giroscópio
static void test_six_and_level_windows() {
    CalCoordinator c(CP);
    uint32_t t = 0;
    c.setMode(CalMode::SixMeasure);
    c.startWindow(t);
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Six, feedWindow(c, t, BIAS, LEVEL_G));
    TEST_ASSERT_FALSE(c.window().active);
    TEST_ASSERT_EQUAL_UINT32(400, c.window().accRaw.count());
    c.setMode(CalMode::LevelMeasure);
    c.startWindow(t);
    TEST_ASSERT_EQUAL(CalCoordinator::Done::Level, feedWindow(c, t, BIAS, LEVEL_G));
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
    RUN_TEST(test_end_six);
    RUN_TEST(test_six_and_level_windows);
    RUN_TEST(test_window_wraparound_and_hold);
    return UNITY_END();
}
