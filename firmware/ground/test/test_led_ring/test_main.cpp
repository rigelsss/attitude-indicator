// Testes do anel de LEDs (lib/led_ring): geometria, cor do horizonte, estados, flash do alarme, potenciômetro e
// teto de corrente. Rodar no PC: pio test -e native
#include <unity.h>
#include <math.h>
#include "led_ring.h"

void setUp() {}
void tearDown() {}

// Caixa da tampa: 12,5 cm (cauda → nariz) × 15,5 cm (esquerda → direita), 9 LEDs no nariz e na cauda, 7 nas
// laterais
static LedRingGeom geom(LedCorner start, bool cw) { return {125.0f, 155.0f, 9, 7, 9, 7, start, cw}; }

static const LedLook LOOK = {
    1.0f, 20.0f,
    {255, 255, 255}, {0, 60, 255}, {150, 60, 0}, {255, 120, 0}, {255, 0, 0},
    500, 3000, 2000, 1500, 0.2f,
};

static bool same(LedRgb a, LedRgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// ---------- Geometria ----------

static void test_layout_count_and_perimeter() {
    LedPoint p[40];
    const LedRingGeom g = geom(LedCorner::TailLeft, true);
    TEST_ASSERT_EQUAL_UINT16(32, ledRingCount(g));
    TEST_ASSERT_EQUAL_UINT16(32, ledRingLayout(g, p, 40));
    for (int i = 0; i < 32; i++) {
        const bool onX = fabsf(fabsf(p[i].xMm) - 62.5f) < 0.01f, onY = fabsf(fabsf(p[i].yMm) - 77.5f) < 0.01f;
        TEST_ASSERT_TRUE_MESSAGE(onX || onY, "LED fora do perímetro");
        TEST_ASSERT_TRUE(fabsf(p[i].xMm) <= 62.51f && fabsf(p[i].yMm) <= 77.51f);
        for (int j = 0; j < i; j++)
            TEST_ASSERT_FALSE(fabsf(p[i].xMm - p[j].xMm) < 0.01f && fabsf(p[i].yMm - p[j].yMm) < 0.01f);
    }
}

static void test_layout_start_and_direction() {
    LedPoint p[32];
    // Horário a partir da cauda-esquerda: sobe pela lateral esquerda em direção ao nariz
    ledRingLayout(geom(LedCorner::TailLeft, true), p, 32);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -77.5f, p[0].yMm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -62.5f + 125.0f * 0.5f / 7.0f, p[0].xMm);
    TEST_ASSERT_TRUE(p[1].xMm > p[0].xMm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 62.5f, p[7].xMm);    // o 8º já está no nariz
    TEST_ASSERT_TRUE(p[8].yMm > p[7].yMm);               // indo para a direita
    // Anti-horário a partir da cauda-esquerda: vai pela cauda, para a direita
    ledRingLayout(geom(LedCorner::TailLeft, false), p, 32);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -62.5f, p[0].xMm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -77.5f + 155.0f * 0.5f / 9.0f, p[0].yMm);
    TEST_ASSERT_TRUE(p[1].yMm > p[0].yMm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 77.5f, p[9].yMm);    // o 10º já está na lateral direita
    // Horário a partir do nariz-direita: desce pela lateral direita
    ledRingLayout(geom(LedCorner::NoseRight, true), p, 32);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 77.5f, p[0].yMm);
    TEST_ASSERT_TRUE(p[1].xMm < p[0].xMm);
}

static void test_sides_order() {
    uint8_t sides[4], counts[4];
    ledRingSides(geom(LedCorner::TailLeft, true), sides, counts);   // esquerda, nariz, direita, cauda
    TEST_ASSERT_EQUAL_UINT8(3, sides[0]);
    TEST_ASSERT_EQUAL_UINT8(0, sides[1]);
    TEST_ASSERT_EQUAL_UINT8(7, counts[0]);
    TEST_ASSERT_EQUAL_UINT8(9, counts[1]);
    ledRingSides(geom(LedCorner::TailLeft, false), sides, counts);  // cauda, direita, nariz, esquerda
    TEST_ASSERT_EQUAL_UINT8(2, sides[0]);
    TEST_ASSERT_EQUAL_UINT8(1, sides[1]);
    TEST_ASSERT_EQUAL_UINT8(0, sides[2]);
    TEST_ASSERT_EQUAL_UINT8(3, sides[3]);
}

static void test_layout_respects_max() {
    LedPoint p[10];
    TEST_ASSERT_EQUAL_UINT16(10, ledRingLayout(geom(LedCorner::NoseLeft, true), p, 10));
}

// ---------- Cor do horizonte ----------

static const LedPoint NOSE = {62.5f, 0.0f}, TAIL = {-62.5f, 0.0f}, RIGHT = {0.0f, 77.5f}, LEFT = {0.0f, -77.5f};

static void test_level_is_white() {
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, NOSE, 0.0f, 0.0f)));
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, RIGHT, 0.0f, 0.0f)));
    // Dentro da faixa morta (1°)
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, NOSE, 0.0f, 0.9f)));
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, RIGHT, -0.9f, 0.0f)));
}

static void test_signs_match_tft() {
    // Nariz para cima: a borda de longe (nariz) fica alta (azul), a de perto (cauda) baixa (marrom)
    TEST_ASSERT_TRUE(same(LOOK.sky, ledHorizonColor(LOOK, NOSE, 0.0f, 30.0f)));
    TEST_ASSERT_TRUE(same(LOOK.ground, ledHorizonColor(LOOK, TAIL, 0.0f, 30.0f)));
    // Roll à direita (asa direita descendo): a borda direita fica marrom, a esquerda azul
    TEST_ASSERT_TRUE(same(LOOK.ground, ledHorizonColor(LOOK, RIGHT, 30.0f, 0.0f)));
    TEST_ASSERT_TRUE(same(LOOK.sky, ledHorizonColor(LOOK, LEFT, 30.0f, 0.0f)));
    // O eixo da rotação não muda: com roll puro, nariz e cauda continuam brancos
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, NOSE, 30.0f, 0.0f)));
}

static void test_color_is_continuous() {
    // A cor sai do branco aos poucos: o azul de 5° fica entre o de 3° e o de 10°, e a 20° está pura
    const LedRgb c3 = ledHorizonColor(LOOK, NOSE, 0.0f, 3.0f);
    const LedRgb c5 = ledHorizonColor(LOOK, NOSE, 0.0f, 5.0f);
    const LedRgb c10 = ledHorizonColor(LOOK, NOSE, 0.0f, 10.0f);
    TEST_ASSERT_TRUE(c3.r > c5.r && c5.r > c10.r && c10.r > 0);
    TEST_ASSERT_TRUE(same(LOOK.sky, ledHorizonColor(LOOK, NOSE, 0.0f, 20.5f)));
    // Pouco acima da faixa morta, quase branco
    const LedRgb c11 = ledHorizonColor(LOOK, NOSE, 0.0f, 1.1f);
    TEST_ASSERT_TRUE(c11.r >= 250);
}

static void test_diagonal_spreads() {
    // Com roll e pitch juntos, o canto nariz-esquerda fica mais alto que o meio do nariz
    const LedPoint noseLeft = {62.5f, -77.5f};
    const LedRgb mid = ledHorizonColor(LOOK, NOSE, 8.0f, 8.0f), corner = ledHorizonColor(LOOK, noseLeft, 8.0f, 8.0f);
    TEST_ASSERT_TRUE(corner.r < mid.r);   // mais azul (menos vermelho do branco)
}

static void test_nan_is_white() {
    TEST_ASSERT_TRUE(same(LOOK.white, ledHorizonColor(LOOK, NOSE, NAN, 0.0f)));
}

// ---------- Flash do alarme ----------

static void test_flash_timing() {
    LedAlarmFlash f;
    TEST_ASSERT_FALSE(f.update(false, 0, 1000, 500, 3000));
    TEST_ASSERT_TRUE(f.update(true, 1, 1000, 500, 3000));    // começa aceso na hora
    TEST_ASSERT_TRUE(f.update(true, 1, 1499, 500, 3000));
    TEST_ASSERT_FALSE(f.update(true, 1, 1500, 500, 3000));
    TEST_ASSERT_FALSE(f.update(true, 1, 3999, 500, 3000));
    TEST_ASSERT_TRUE(f.update(true, 1, 4000, 500, 3000));    // 3 s depois, de novo
    // O tipo mudou (aviso → alarme): recomeça aceso
    TEST_ASSERT_TRUE(f.update(true, 2, 4800, 500, 3000));
    TEST_ASSERT_FALSE(f.update(true, 2, 5300, 500, 3000));
    // Reconhecido (inativo) e de volta: recomeça aceso
    TEST_ASSERT_FALSE(f.update(false, 2, 6000, 500, 3000));
    TEST_ASSERT_TRUE(f.update(true, 2, 6100, 500, 3000));
}

// ---------- Quadro ----------

static LedPoint s_pts[64];
static const LedRingSpan RINGS[2] = {{0, 32, 0.7f}, {32, 32, 1.0f}};

static void layoutBoth() {
    ledRingLayout(geom(LedCorner::TailLeft, true), s_pts, 32);
    ledRingLayout(geom(LedCorner::TailLeft, true), s_pts + 32, 32);
}

static void test_render_nodata_spinner() {
    layoutBoth();
    LedRgb out[64];
    const LedFrameInput in = {LedMode::NoData, 0, 0, false};
    ledRenderFrame(LOOK, s_pts, RINGS, 2, in, 0, out);
    int lit0 = 0, lit1 = 0;
    for (int i = 0; i < 32; i++) lit0 += out[i].r || out[i].g || out[i].b;
    for (int i = 32; i < 64; i++) lit1 += out[i].r || out[i].g || out[i].b;
    TEST_ASSERT_EQUAL_INT(1, lit0);
    TEST_ASSERT_EQUAL_INT(1, lit1);
    TEST_ASSERT_EQUAL_UINT8(255, out[32].r);              // vermelho cheio no lateral, LED 0 no instante 0
    TEST_ASSERT_EQUAL_UINT8(179, out[0].r);               // 70 % no de cima
    // Meia volta depois, o LED aceso está na metade do anel
    ledRenderFrame(LOOK, s_pts, RINGS, 2, in, 1000, out);
    TEST_ASSERT_EQUAL_UINT8(255, out[32 + 16].r);
}

static void test_render_states() {
    layoutBoth();
    LedRgb out[64];
    ledRenderFrame(LOOK, s_pts, RINGS, 2, {LedMode::AttInvalid, 0, 0, false}, 0, out);
    TEST_ASSERT_TRUE(same(LOOK.amber, out[40]));
    ledRenderFrame(LOOK, s_pts, RINGS, 2, {LedMode::Calibrating, 0, 0, false}, 0, out);
    TEST_ASSERT_EQUAL_UINT8(51, out[40].r);               // início do pulso: 20 % do âmbar
    ledRenderFrame(LOOK, s_pts, RINGS, 2, {LedMode::Calibrating, 0, 0, false}, 750, out);
    TEST_ASSERT_EQUAL_UINT8(255, out[40].r);              // meio do ciclo: cheio
    ledRenderFrame(LOOK, s_pts, RINGS, 2, {LedMode::Horizon, 0, 0, true}, 0, out);
    for (int i = 32; i < 64; i++) TEST_ASSERT_TRUE(same(LOOK.alarm, out[i]));   // flash: anel inteiro vermelho
    ledRenderFrame(LOOK, s_pts, RINGS, 2, {LedMode::Horizon, 0, 0, false}, 0, out);
    TEST_ASSERT_TRUE(same(LOOK.white, out[40]));
}

// ---------- Potenciômetro ----------

static const LedPotConfig POT = {4095, 40, 2.2f, 0.05f};

static void test_pot_ends_and_curve() {
    LedPot pot(POT);
    for (int i = 0; i < 8; i++) pot.update(4095);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, pot.brightness());
    for (int i = 0; i < 8; i++) pot.update(0);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.05f, pot.brightness());   // nunca some
    for (int i = 0; i < 8; i++) pot.update(2048);
    // Metade do curso: bem abaixo da metade do brilho (curva de gama), acima do mínimo
    TEST_ASSERT_TRUE(pot.brightness() > 0.15f && pot.brightness() < 0.35f);
    // Quase no fim, com ruído: encosta no máximo
    for (int i = 0; i < 8; i++) pot.update(4070);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, pot.brightness());
}

static void test_pot_hysteresis() {
    LedPot pot(POT);
    for (int i = 0; i < 8; i++) pot.update(2000);
    const float b = pot.brightness();
    // Ruído de ±30 contagens: o brilho não muda
    for (int i = 0; i < 40; i++) pot.update((uint16_t)(2000 + ((i & 1) ? 30 : -30)));
    TEST_ASSERT_EQUAL_FLOAT(b, pot.brightness());
    // Girou de verdade: muda
    for (int i = 0; i < 8; i++) pot.update(2600);
    TEST_ASSERT_TRUE(pot.brightness() > b);
}

// ---------- Brilho e teto ----------

static const LedPowerConfig POWER = {20.0f, 1.0f, 1300.0f};

static void test_brightness_scales() {
    LedRgb buf[2] = {{200, 100, 0}, {0, 0, 255}};
    const LedPowerResult r = ledApplyBrightness(buf, 2, 0.5f, POWER);
    TEST_ASSERT_FALSE(r.capped);
    TEST_ASSERT_EQUAL_UINT8(100, buf[0].r);
    TEST_ASSERT_EQUAL_UINT8(50, buf[0].g);
    TEST_ASSERT_EQUAL_UINT8(128, buf[1].b);
}

static void test_power_cap() {
    // 65 LEDs em branco cheio: ~3,9 A estimados, reduzidos para até 1,3 A
    LedRgb buf[65];
    for (int i = 0; i < 65; i++) buf[i] = {255, 255, 255};
    const LedPowerResult r = ledApplyBrightness(buf, 65, 1.0f, POWER);
    TEST_ASSERT_TRUE(r.capped);
    TEST_ASSERT_TRUE(r.estimatedMa <= 1300.0f && r.estimatedMa > 1250.0f);
    TEST_ASSERT_TRUE(buf[0].r > 60 && buf[0].r < 100);    // ~1/3 de cada canal
    TEST_ASSERT_EQUAL_UINT8(buf[0].r, buf[64].b);          // o quadro inteiro na mesma proporção
    // Vermelho cheio (um canal): ~1,37 A, um pouco acima do teto
    for (int i = 0; i < 65; i++) buf[i] = {255, 0, 0};
    const LedPowerResult r2 = ledApplyBrightness(buf, 65, 1.0f, POWER);
    TEST_ASSERT_TRUE(r2.capped && r2.estimatedMa <= 1300.0f);
    // Abaixo do teto, nada muda
    for (int i = 0; i < 65; i++) buf[i] = {0, 0, 100};
    const LedPowerResult r3 = ledApplyBrightness(buf, 65, 1.0f, POWER);
    TEST_ASSERT_FALSE(r3.capped);
    TEST_ASSERT_EQUAL_UINT8(100, buf[10].b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_layout_count_and_perimeter);
    RUN_TEST(test_layout_start_and_direction);
    RUN_TEST(test_sides_order);
    RUN_TEST(test_layout_respects_max);
    RUN_TEST(test_level_is_white);
    RUN_TEST(test_signs_match_tft);
    RUN_TEST(test_color_is_continuous);
    RUN_TEST(test_diagonal_spreads);
    RUN_TEST(test_nan_is_white);
    RUN_TEST(test_flash_timing);
    RUN_TEST(test_render_nodata_spinner);
    RUN_TEST(test_render_states);
    RUN_TEST(test_pot_ends_and_curve);
    RUN_TEST(test_pot_hysteresis);
    RUN_TEST(test_brightness_scales);
    RUN_TEST(test_power_cap);
    return UNITY_END();
}
