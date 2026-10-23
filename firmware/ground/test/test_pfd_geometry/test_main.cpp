// Testes da geometria do horizonte (lib/pfd_geometry).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "pfd_geometry.h"

void setUp() {}
void tearDown() {}

static const PfdConfig CFG = {120.0f, 120.0f, 240.0f, 240.0f, 4.0f};
static const float TOL = 0.01f;

static float area(const Vec2 *v, int n) {
    float a = 0;
    for (int i = 0; i < n; i++) a += v[i].x * v[(i + 1) % n].y - v[(i + 1) % n].x * v[i].y;
    return a < 0 ? -a / 2 : a / 2;
}

// Nivelado: horizonte no centro, terra na metade de baixo
static void test_level() {
    const Horizon h = pfdHorizon(CFG, 0, 0);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 120.0f, h.point.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 120.0f, h.point.y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0f, h.dir.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, h.dir.y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0f, h.down.y);
    Vec2 g[6];
    const int n = pfdGroundPolygon(CFG, h, g);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 240.0f * 120.0f, area(g, n));
    for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(g[i].y >= 120.0f - TOL);
}

// Nariz para cima 10°: o horizonte desce 40 px
static void test_pitch_up_moves_horizon_down() {
    const Horizon h = pfdHorizon(CFG, 0, 10);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 160.0f, h.point.y);
    Vec2 g[6];
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 240.0f * 80.0f, area(g, pfdGroundPolygon(CFG, h, g)));
    const Horizon h2 = pfdHorizon(CFG, 0, -10);   // nariz para baixo: sobe
    TEST_ASSERT_FLOAT_WITHIN(TOL, 80.0f, h2.point.y);
}

// Roll à direita: o horizonte gira no sentido anti-horário na tela (lado esquerdo desce)
static void test_roll_right_direction() {
    const Horizon h = pfdHorizon(CFG, 30, 0);
    // Pontos do horizonte à esquerda e à direita do centro
    const Vec2 left = {h.point.x - h.dir.x * 100, h.point.y - h.dir.y * 100};
    const Vec2 right = {h.point.x + h.dir.x * 100, h.point.y + h.dir.y * 100};
    TEST_ASSERT_TRUE(left.y > 120.0f);    // esquerda desce
    TEST_ASSERT_TRUE(right.y < 120.0f);   // direita sobe
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f + 50.0f, left.y);   // 100·sen 30° = 50
    // Roll à esquerda: o contrário
    const Horizon h2 = pfdHorizon(CFG, -30, 0);
    TEST_ASSERT_TRUE(h2.point.y - h2.dir.y * 100 < 120.0f);
}

// Roll de 90°: a terra fica do lado direito
static void test_roll_90_ground_right() {
    const Horizon h = pfdHorizon(CFG, 90, 0);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0f, h.down.x);
    Vec2 g[6];
    const int n = pfdGroundPolygon(CFG, h, g);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 120.0f * 240.0f, area(g, n));
    for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(g[i].x >= 120.0f - 0.5f);
}

// Dorso: a terra fica em cima
static void test_inverted_ground_on_top() {
    const Horizon h = pfdHorizon(CFG, 180, 0);
    Vec2 g[6];
    const int n = pfdGroundPolygon(CFG, h, g);
    for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(g[i].y <= 120.0f + 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 240.0f * 120.0f, area(g, n));
}

// Pitch fora da tela: só céu ou só terra
static void test_off_screen() {
    Vec2 g[6];
    TEST_ASSERT_EQUAL_INT(0, pfdGroundPolygon(CFG, pfdHorizon(CFG, 0, 40), g));   // horizonte abaixo: só céu
    const int n = pfdGroundPolygon(CFG, pfdHorizon(CFG, 0, -40), g);                 // acima: só terra
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 240.0f * 240.0f, area(g, n));
}

// Roll inclinado corta um canto: o polígono tem 5 vértices e área coerente
static void test_corner_cut() {
    const Horizon h = pfdHorizon(CFG, 30, -20);   // horizonte acima do centro e inclinado
    Vec2 g[6];
    const int n = pfdGroundPolygon(CFG, h, g);
    TEST_ASSERT_TRUE(n >= 3 && n <= 5);
    TEST_ASSERT_TRUE(area(g, n) > 240.0f * 120.0f);   // a terra ocupa mais da metade
}

static void test_clip_segment() {
    Vec2 a = {-100, 120}, b = {340, 120};
    TEST_ASSERT_TRUE(pfdClipSegment(CFG, a, b));
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, a.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 240.0f, b.x);
    Vec2 c = {-10, -10}, d = {-5, 300};   // inteiro à esquerda
    TEST_ASSERT_FALSE(pfdClipSegment(CFG, c, d));
    Vec2 e = {10, 10}, f = {20, 30};   // inteiro dentro: não muda
    TEST_ASSERT_TRUE(pfdClipSegment(CFG, e, f));
    TEST_ASSERT_FLOAT_WITHIN(TOL, 20.0f, f.x);
}

// Marca de +10°: 40 px para o lado do céu, paralela ao horizonte
static void test_pitch_mark() {
    Vec2 c, a, b;
    pfdPitchMark(CFG, pfdHorizon(CFG, 0, 0), 10, 30, c, a, b);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 80.0f, c.y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 90.0f, a.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 150.0f, b.x);
    // Com o nariz 10° para cima, a marca de +10° fica no centro (alinhada com o avião)
    pfdPitchMark(CFG, pfdHorizon(CFG, 0, 10), 10, 30, c, a, b);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 120.0f, c.y);
}

// Escala de roll fixa e ponteiro do lado do céu
static void test_roll_scale_and_pointer() {
    const Vec2 top = pfdRollScalePoint(CFG, 0, 100);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 120.0f, top.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 20.0f, top.y);
    const Vec2 r30 = pfdRollScalePoint(CFG, 30, 100);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 170.0f, r30.x);   // 100·sen 30°
    // Nivelado: ponteiro no zero da escala
    const Vec2 p0 = pfdRollPointer(CFG, pfdHorizon(CFG, 0, 0), 100);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 120.0f, p0.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 20.0f, p0.y);
    // Roll à direita: o ponteiro vai para a esquerda, na marca de −30° da escala
    const Vec2 p30 = pfdRollPointer(CFG, pfdHorizon(CFG, 30, 0), 100);
    const Vec2 m30 = pfdRollScalePoint(CFG, -30, 100);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, m30.x, p30.x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, m30.y, p30.y);
    // O pitch não move o ponteiro
    const Vec2 pp = pfdRollPointer(CFG, pfdHorizon(CFG, 30, 15), 100);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, p30.x, pp.x);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_level);
    RUN_TEST(test_pitch_up_moves_horizon_down);
    RUN_TEST(test_roll_right_direction);
    RUN_TEST(test_roll_90_ground_right);
    RUN_TEST(test_inverted_ground_on_top);
    RUN_TEST(test_off_screen);
    RUN_TEST(test_corner_cut);
    RUN_TEST(test_clip_segment);
    RUN_TEST(test_pitch_mark);
    RUN_TEST(test_roll_scale_and_pointer);
    return UNITY_END();
}
