// Testes da decodificação do encoder e do botão (lib/encoder_decode).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "encoder_decode.h"

void setUp() {}
void tearDown() {}

static const EncoderConfig CFG = {2, 1};

// Estados (CLK << 1) | DT. Horário (positivo): 11 → 01 → 00 → 10 → 11
static const uint8_t S11 = 3, S01 = 1, S00 = 0, S10 = 2;

static int feedAll(EncoderDecoder &e, const uint8_t *states, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) sum += e.feed(states[i]);
    return sum;
}

static void test_clockwise_clicks() {
    EncoderDecoder e(CFG, S11);
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S01));
    TEST_ASSERT_EQUAL_INT8(+1, e.feed(S00));   // clique, repouso em 00
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S10));
    TEST_ASSERT_EQUAL_INT8(+1, e.feed(S11));   // clique, repouso em 11
    TEST_ASSERT_EQUAL_UINT32(0, e.invalid());
}

static void test_counterclockwise_clicks() {
    EncoderDecoder e(CFG, S11);
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S10));
    TEST_ASSERT_EQUAL_INT8(-1, e.feed(S00));
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S01));
    TEST_ASSERT_EQUAL_INT8(-1, e.feed(S11));
}

// Partindo do repouso em 00
static void test_start_at_00() {
    EncoderDecoder e(CFG, S00);
    const uint8_t cw[] = {S10, S11, S01, S00};
    TEST_ASSERT_EQUAL_INT(2, feedAll(e, cw, 4));
}

// Meio clique e volta: nenhum clique
static void test_half_click_and_back() {
    EncoderDecoder e(CFG, S11);
    const uint8_t seq[] = {S01, S11};
    TEST_ASSERT_EQUAL_INT(0, feedAll(e, seq, 2));
    // O seguinte, completo, conta normalmente
    const uint8_t cw[] = {S01, S00};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, cw, 2));
}

// Repique em cada borda (ida e volta antes de firmar): um clique só
static void test_bounce_on_edges() {
    EncoderDecoder e(CFG, S11);
    const uint8_t seq[] = {S01, S11, S01, S00, S01, S00};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, seq, 6));
    TEST_ASSERT_EQUAL_UINT32(0, e.invalid());
}

// Estado repetido (duas bordas lidas no mesmo estado) não muda nada
static void test_repeated_state() {
    EncoderDecoder e(CFG, S11);
    const uint8_t seq[] = {S11, S01, S01, S00, S00};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, seq, 5));
}

// Transição que pula um estado: inválida, ignorada, sem clique falso
static void test_invalid_transition() {
    EncoderDecoder e(CFG, S11);
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S00));   // 11 → 00 pula um estado
    TEST_ASSERT_EQUAL_UINT32(1, e.invalid());
    // Recomeça do 00 normalmente
    const uint8_t cw[] = {S10, S11};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, cw, 2));
}

// Inválida depois de um passo parcial (11 → 01 → 10 → 11): a soma zera, sem clique combinado
static void test_invalid_after_partial_step() {
    EncoderDecoder e(CFG, S11);
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S01));   // +1 parcial
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S10));   // 01 → 10 pula um estado
    TEST_ASSERT_EQUAL_UINT32(1, e.invalid());
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S11));   // +1, mas a soma recomeçou: só ressincroniza no repouso
    // O próximo clique completo conta
    const uint8_t cw[] = {S01, S00};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, cw, 2));
}

// Depois de uma inválida fora do repouso, os passos até o repouso seguinte não contam
static void test_invalid_waits_for_detent() {
    EncoderDecoder e(CFG, S00);
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S11));   // 00 → 11: inválida, mas 11 é repouso: ressincroniza ali
    const uint8_t cw[] = {S01, S00};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, cw, 2));
    EncoderDecoder f(CFG, S01);
    TEST_ASSERT_EQUAL_INT8(0, f.feed(S10));   // 01 → 10: inválida fora do repouso
    TEST_ASSERT_EQUAL_INT8(0, f.feed(S00));   // passo válido, ainda sem repouso anterior: não soma
    const uint8_t cw2[] = {S10, S11};
    TEST_ASSERT_EQUAL_INT(1, feedAll(f, cw2, 2));
}

// Lacuna (fila da ISR cheia): giro horário com 01 e 00 perdidos. Sem resync, 11 → 10 seria −1
static void test_gap_does_not_invert() {
    // Sem o marcador: um giro horário com estados perdidos pode chegar como 11 → 10 → 00, que forma um clique
    // anti-horário falso (o defeito que o resync evita)
    EncoderDecoder plain(CFG, S11);
    TEST_ASSERT_EQUAL_INT8(0, plain.feed(S10));
    TEST_ASSERT_EQUAL_INT8(-1, plain.feed(S00));
    EncoderDecoder e(CFG, S11);
    e.resync();                                   // marcador de lacuna antes do 10
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S10));       // só registra
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S11));       // repouso: ressincroniza, sem clique
    const uint8_t cw[] = {S01, S00, S10, S11};
    TEST_ASSERT_EQUAL_INT(2, feedAll(e, cw, 4));  // o giro horário segue positivo
}

// Lacuna com o primeiro estado já num repouso: a contagem recomeça dali
static void test_gap_landing_on_detent() {
    EncoderDecoder e(CFG, S11);
    e.feed(S01);   // parcial
    e.resync();
    TEST_ASSERT_EQUAL_INT8(0, e.feed(S00));   // repouso
    const uint8_t cw[] = {S10, S11};
    TEST_ASSERT_EQUAL_INT(1, feedAll(e, cw, 2));
    TEST_ASSERT_EQUAL_UINT32(0, e.invalid());
}

// Giro rápido: muitos cliques seguidos, nos dois sentidos
static void test_fast_spin() {
    EncoderDecoder e(CFG, S11);
    const uint8_t cycle[] = {S01, S00, S10, S11};
    int sum = 0;
    for (int i = 0; i < 25; i++) sum += feedAll(e, cycle, 4);
    TEST_ASSERT_EQUAL_INT(50, sum);
    const uint8_t back[] = {S10, S00, S01, S11};
    sum = 0;
    for (int i = 0; i < 25; i++) sum += feedAll(e, back, 4);
    TEST_ASSERT_EQUAL_INT(-50, sum);
}

static void test_direction_inverted() {
    const EncoderConfig inv = {2, -1};
    EncoderDecoder e(inv, S11);
    const uint8_t cw[] = {S01, S00};
    TEST_ASSERT_EQUAL_INT(-1, feedAll(e, cw, 2));
}

// Encoder de 4 passos por clique: repouso só em 11
static void test_four_steps_per_click() {
    const EncoderConfig four = {4, 1};
    EncoderDecoder e(four, S11);
    const uint8_t cycle[] = {S01, S00, S10, S11};
    TEST_ASSERT_EQUAL_INT8(0, e.feed(cycle[0]));
    TEST_ASSERT_EQUAL_INT8(0, e.feed(cycle[1]));   // 00 não é repouso aqui
    TEST_ASSERT_EQUAL_INT8(0, e.feed(cycle[2]));
    TEST_ASSERT_EQUAL_INT8(+1, e.feed(cycle[3]));
}

// ---------- Botão ----------

static void test_button_short() {
    ButtonDebounce b(20, 1000);
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(true, 0));
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(true, 20));   // firmou pressionado
    TEST_ASSERT_TRUE(b.pressed());
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(false, 200));
    TEST_ASSERT_EQUAL(ButtonEvent::Short, b.update(false, 220));   // firmou solto
}

static void test_button_long() {
    ButtonDebounce b(20, 1000);
    b.update(true, 0);
    b.update(true, 20);
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(true, 1019));
    TEST_ASSERT_EQUAL(ButtonEvent::Long, b.update(true, 1020));   // ainda pressionado
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(true, 1500));   // uma vez só
    b.update(false, 2000);
    TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(false, 2020));   // sem curto ao soltar
    // O próximo aperto curto funciona
    b.update(true, 3000);
    b.update(true, 3020);
    b.update(false, 3100);
    TEST_ASSERT_EQUAL(ButtonEvent::Short, b.update(false, 3120));
}

// Repique na pressão e na soltura: um curto só
static void test_button_bounce() {
    ButtonDebounce b(20, 1000);
    int shorts = 0;
    const bool press[] = {true, false, true, false, true};
    uint32_t t = 0;
    for (bool p : press) {
        if (b.update(p, t) == ButtonEvent::Short) shorts++;
        t += 3;
    }
    for (; t < 200; t += 5)
        if (b.update(true, t) == ButtonEvent::Short) shorts++;
    const bool release[] = {false, true, false, true, false};
    for (bool p : release) {
        if (b.update(p, t) == ButtonEvent::Short) shorts++;
        t += 3;
    }
    for (; t < 400; t += 5)
        if (b.update(false, t) == ButtonEvent::Short) shorts++;
    TEST_ASSERT_EQUAL_INT(1, shorts);
}

// Pulso mais curto que o debounce: ignorado
static void test_button_glitch_ignored() {
    ButtonDebounce b(20, 1000);
    b.update(true, 0);
    b.update(false, 10);
    for (uint32_t t = 10; t < 200; t += 5) TEST_ASSERT_EQUAL(ButtonEvent::None, b.update(false, t));
    TEST_ASSERT_FALSE(b.pressed());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_clockwise_clicks);
    RUN_TEST(test_counterclockwise_clicks);
    RUN_TEST(test_start_at_00);
    RUN_TEST(test_half_click_and_back);
    RUN_TEST(test_bounce_on_edges);
    RUN_TEST(test_repeated_state);
    RUN_TEST(test_invalid_transition);
    RUN_TEST(test_invalid_after_partial_step);
    RUN_TEST(test_invalid_waits_for_detent);
    RUN_TEST(test_gap_does_not_invert);
    RUN_TEST(test_gap_landing_on_detent);
    RUN_TEST(test_fast_spin);
    RUN_TEST(test_direction_inverted);
    RUN_TEST(test_four_steps_per_click);
    RUN_TEST(test_button_short);
    RUN_TEST(test_button_long);
    RUN_TEST(test_button_bounce);
    RUN_TEST(test_button_glitch_ignored);
    return UNITY_END();
}
