// Testes da contabilidade da aquisição (lib/acq). Rodar no PC: pio test -e native
#include <math.h>
#include <unity.h>
#include "acq.h"

void setUp() {}
void tearDown() {}

static const uint32_t WINDOW_US = 1000000;   // MAIN_PRINT_MS = 1000
static const float PERIOD_US = 5000.0f;       // 200 Hz

// Como o loop(): fecha as janelas vencidas até o instante do pulso e conta o pulso.
// Devolve quantas janelas fechou.
static int notePulse(PulseTracker &p, uint32_t t) {
    int closed = 0;
    while (p.due(t)) {
        p.rotate();
        closed++;
    }
    p.notePulse(t, PERIOD_US);
    return closed;
}

// 200 pulsos por janela: a janela seguinte começa no fim da anterior, não no instante do fechamento
static void test_windows_continuous_limits() {
    PulseTracker p(WINDOW_US);
    p.begin(1000);
    for (uint32_t i = 0; i < 200; i++) notePulse(p, 1000 + 2500 + i * 5000);
    TEST_ASSERT_EQUAL_UINT32(200, p.cur().pulses);
    // Fechamento atrasado: o próximo pulso chega 7 ms depois do fim da janela
    TEST_ASSERT_EQUAL_INT(1, notePulse(p, 1000 + WINDOW_US + 7000));
    TEST_ASSERT_EQUAL_UINT32(200, p.last().pulses);
    TEST_ASSERT_EQUAL_UINT32(1000 + WINDOW_US, p.cur().startUs);   // e não o instante do fechamento
    TEST_ASSERT_EQUAL_UINT32(1, p.cur().pulses);
    // Intervalo de 9,5 ms desde o último pulso (1,9 período): uma perdida
    TEST_ASSERT_EQUAL_UINT32(1, p.cur().lost);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 200.0f, summarize(p.last(), WINDOW_US).rateHz);
}

// Parada longa (3,5 s sem pulsos): fecha as janelas vencidas de uma vez; as vazias ficam com 0 pulsos
static void test_long_stall_closes_several_windows() {
    PulseTracker p(WINDOW_US);
    p.begin(0);
    notePulse(p, 5000);
    TEST_ASSERT_EQUAL_INT(3, notePulse(p, 3500000));
    TEST_ASSERT_EQUAL_UINT32(0, p.last().pulses);   // janela [2 s, 3 s), vazia
    TEST_ASSERT_EQUAL_UINT32(2000000, p.last().startUs);
    TEST_ASSERT_EQUAL_UINT32(3000000, p.cur().startUs);
    // 3,495 s / 5 ms = 699 períodos: 698 pulsos faltaram, contados na janela do pulso que chegou
    TEST_ASSERT_EQUAL_UINT32(698, p.cur().lost);
}

// Fechamento pelo relógio (sem pulsos), como no INT solto: due/rotate sem notePulse
static void test_close_by_clock() {
    PulseTracker p(WINDOW_US);
    p.begin(0);
    TEST_ASSERT_FALSE(p.due(999999));
    TEST_ASSERT_TRUE(p.due(1000000));
    p.rotate();
    TEST_ASSERT_EQUAL_UINT32(0, p.last().pulses);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, summarize(p.last(), WINDOW_US).rateHz);
}

// Pulso anterior ao início da janela (esperou na fila enquanto ela avançou pelo relógio): fora da
// taxa da janela atual, mas ainda conta para as perdidas
static void test_pulse_before_window_not_counted() {
    PulseTracker p(WINDOW_US);
    p.begin(0);
    notePulse(p, 995000);
    p.rotate();   // fechada pelo relógio
    p.notePulse(1000000 - 1000, PERIOD_US);   // pulso de 999 ms, atendido depois do fechamento
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().pulses);
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);   // 4 ms depois do anterior: sem perdida
    p.notePulse(1004000, PERIOD_US);
    TEST_ASSERT_EQUAL_UINT32(1, p.cur().pulses);
}

// Primeiro pulso depois do begin() não conta perdidas; lacuna de 3 períodos dá 2 perdidas
static void test_lost_pulses() {
    PulseTracker p(WINDOW_US);
    p.begin(0);
    notePulse(p, 400000);   // primeiro: sem referência anterior
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);
    notePulse(p, 405000);
    notePulse(p, 412000);   // 1,4 período: ainda não é perda
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);
    notePulse(p, 427000);   // 3 períodos
    TEST_ASSERT_EQUAL_UINT32(2, p.cur().lost);
    // MPU reconfigurado: sem referência de novo
    p.begin(500000);
    notePulse(p, 900000);
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);
    TEST_ASSERT_EQUAL_UINT32(500000, p.cur().startUs);
}

// Descartadas contam na janela do pulso que ficou sem leitura, antes de avançar
static void test_stale_in_window_of_pulse() {
    PulseTracker p(WINDOW_US);
    p.begin(0);
    notePulse(p, 995000);
    // Dois pulsos na fila: o de 995 ms (já contado) ficou sem leitura; o de 1 000 ms abre a janela nova
    p.noteStale();
    TEST_ASSERT_EQUAL_INT(1, notePulse(p, 1000000));
    TEST_ASSERT_EQUAL_UINT32(1, p.last().stale);
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().stale);
}

// Virada do micros() no meio de uma janela e entre pulsos
static void test_wraparound() {
    PulseTracker p(WINDOW_US);
    const uint32_t t0 = 0xFFFFFFFFu - 500000;
    p.begin(t0);
    uint32_t t = t0 - 2500;   // pulsos no meio dos períodos: o 200º ainda dentro da janela
    for (int i = 0; i < 200; i++) {
        t += 5000;
        TEST_ASSERT_EQUAL_INT(0, notePulse(p, t));
    }
    TEST_ASSERT_EQUAL_UINT32(200, p.cur().pulses);
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);
    t += 5000;
    TEST_ASSERT_EQUAL_INT(1, notePulse(p, t));
    TEST_ASSERT_EQUAL_UINT32(200, p.last().pulses);
    TEST_ASSERT_EQUAL_UINT32(t0 + WINDOW_US, p.cur().startUs);
    t += 15000;   // 3 períodos, depois da virada
    notePulse(p, t);
    TEST_ASSERT_EQUAL_UINT32(2, p.cur().lost);
}

// Pulso exatamente em 0 µs (virada do micros()): continua valendo como referência para as perdidas
static void test_pulse_at_zero_counts_lost() {
    PulseTracker p(WINDOW_US);
    p.begin(0xFFFFFFFFu - 100000);
    notePulse(p, 0xFFFFFFFFu - 4999);   // 5 ms antes de 0
    notePulse(p, 0);
    TEST_ASSERT_EQUAL_UINT32(0, p.cur().lost);
    notePulse(p, 15000);   // 3 períodos depois do pulso em 0: 2 perdidas
    TEST_ASSERT_EQUAL_UINT32(2, p.cur().lost);
    TEST_ASSERT_EQUAL_UINT32(3, p.cur().pulses);
}

// Médias, desvios, pico, saturações e temperatura de valores conhecidos
static void test_sample_window_summary() {
    SampleWindow w;
    w.pulses = 200;
    // Dois valores alternados por eixo: média no meio, desvio = meia diferença
    for (int i = 0; i < 100; i++) {
        w.add({0.0f, 0.0f, 0.99f}, {-1.0f, 2.0f, 0.5f}, 25.0f, false, false);
        w.add({0.0f, 0.0f, 1.01f}, {-3.0f, 2.0f, 0.5f}, 27.0f, i == 0, i < 3);
    }
    TEST_ASSERT_EQUAL_UINT32(200, w.samples);
    TEST_ASSERT_EQUAL_UINT32(1, w.satAcc);
    TEST_ASSERT_EQUAL_UINT32(3, w.satGyro);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, w.peakGyro);
    const WindowSummary s = summarize(w, WINDOW_US);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 200.0f, s.rateHz);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.0, s.mean[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, -2.0, s.mean[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.0, s.accNorm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 0.01, s.accMaxStdG);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.0, s.gyroMaxStdDps);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 26.0f, s.tempC);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 26.0f, w.meanTempC());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, w.meanAcc().z);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, w.meanGyro().y);
    w.addAttitude(-0.5f, 0.8f);
    w.addAttitude(-0.7f, 0.8f);
    TEST_ASSERT_EQUAL_UINT32(2, w.att[0].count());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.6f, w.att[0].mean());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.8f, w.att[1].mean());
}

// Janela sem leituras: só a taxa, sem divisão por zero
static void test_empty_window_summary() {
    SampleWindow w;
    w.pulses = 37;
    const WindowSummary s = summarize(w, WINDOW_US);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 37.0f, s.rateHz);
    TEST_ASSERT_TRUE(isfinite(s.accNorm));
    TEST_ASSERT_TRUE(isfinite(s.tempC));
}

// Validade temporal: válido; pulso mais novo durante a leitura; atraso acima do teto; virada
static void test_check_frame() {
    FrameCheck f = checkFrame(1000, 1112, false, 4000);
    TEST_ASSERT_TRUE(f.valid);
    TEST_ASSERT_EQUAL_UINT32(112, f.lagUs);
    TEST_ASSERT_FALSE(checkFrame(1000, 1112, true, 4000).valid);
    TEST_ASSERT_TRUE(checkFrame(1000, 5000, false, 4000).valid);   // exatamente no teto
    f = checkFrame(1000, 5001, false, 4000);
    TEST_ASSERT_FALSE(f.valid);
    TEST_ASSERT_EQUAL_UINT32(4001, f.lagUs);
    f = checkFrame(0xFFFFFFF0u, 0x60, false, 4000);   // leitura depois da virada
    TEST_ASSERT_TRUE(f.valid);
    TEST_ASSERT_EQUAL_UINT32(0x70, f.lagUs);
}

// Estatística do fluxo a: o maior atraso só dos quadros válidos; os inválidos contam como ambíguos
static void test_acq_stats() {
    AcqStats a;
    a.noteFrame({true, 110});
    a.noteFrame({true, 125});
    a.noteFrame({true, 117});
    a.noteFrame({false, 9000});
    TEST_ASSERT_EQUAL_UINT32(125, a.lagMaxUs);
    TEST_ASSERT_EQUAL_UINT32(1, a.ambiguous);
}

// Descarte de partida: segura até o prazo, solta e não volta a segurar (nem ~36 min depois)
static void test_settle_gate() {
    SettleGate g;
    TEST_ASSERT_FALSE(g.hold(0));   // sem begin(): nada a descartar
    g.begin(1000, 200000);
    TEST_ASSERT_TRUE(g.active());
    TEST_ASSERT_TRUE(g.hold(1000));
    TEST_ASSERT_TRUE(g.hold(200999));
    TEST_ASSERT_FALSE(g.hold(201000));
    TEST_ASSERT_FALSE(g.active());
    // ~36 min depois a diferença com sinal ficaria negativa de novo; o estado explícito impede
    TEST_ASSERT_FALSE(g.hold(201000 + 0x80000000u));
    // Nova configuração: segura de novo, também na virada do micros()
    g.begin(0xFFFFFFFFu - 100000, 200000);
    TEST_ASSERT_TRUE(g.hold(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(g.hold(50000));
    TEST_ASSERT_FALSE(g.hold(100000));
}

// ---------- Pulsos falsos (PulseFilter, pendência 8) ----------

// Pulsos certos a cada 5 ms: todos aceitos
static void test_pulse_filter_accepts_period() {
    PulseFilter f(2500);
    for (uint32_t k = 0; k < 10; k++) TEST_ASSERT_TRUE(f.accept(1000 + 5000 * k));
}

// Rajada a 0,3 ms (bordas captadas com o fio meio encaixado): só a primeira passa
static void test_pulse_filter_rejects_burst() {
    PulseFilter f(2500);
    TEST_ASSERT_TRUE(f.accept(0));
    int accepted = 0;
    for (uint32_t t = 300; t < 2500; t += 300) accepted += f.accept(t);
    TEST_ASSERT_EQUAL(0, accepted);
    TEST_ASSERT_TRUE(f.accept(5000));   // o certo seguinte: 5 ms depois do último aceito
}

// Um falso 1 ms depois do certo é recusado e não vira referência: o certo seguinte, 5 ms depois, passa
static void test_pulse_filter_isolated_spurious() {
    PulseFilter f(2500);
    TEST_ASSERT_TRUE(f.accept(10000));
    TEST_ASSERT_FALSE(f.accept(11000));
    TEST_ASSERT_TRUE(f.accept(15000));
}

// Exatamente no intervalo mínimo é aceito; 1 µs antes, não
static void test_pulse_filter_boundary() {
    PulseFilter f(2500);
    TEST_ASSERT_TRUE(f.accept(0));
    TEST_ASSERT_FALSE(f.accept(2499));
    TEST_ASSERT_TRUE(f.accept(2500));
}

// reset(): sem referência, o primeiro pulso é aceito mesmo perto do anterior
static void test_pulse_filter_reset() {
    PulseFilter f(2500);
    TEST_ASSERT_TRUE(f.accept(0));
    f.reset();
    TEST_ASSERT_TRUE(f.accept(100));
    TEST_ASSERT_FALSE(f.accept(200));
}

// Virada do micros() (~71 min): a diferença sem sinal continua certa
static void test_pulse_filter_wraparound() {
    PulseFilter f(2500);
    TEST_ASSERT_TRUE(f.accept(0xFFFFF000u));   // 4 096 µs antes da virada
    TEST_ASSERT_FALSE(f.accept(0xFFFFF800u));  // 2 048 µs depois: falso
    TEST_ASSERT_TRUE(f.accept(0x00000388u));   // 5 000 µs depois do aceito, já depois da virada
}

// ---------- Quarentena depois de um pulso falso (SpuriousHold) ----------

// A brecha da bancada de 21/10: falso aceito em G+4 ms, verdadeiro de G+5 recusado (a 1 ms do falso), falso
// seguinte em G+7 aceito e lido com a amostra de G+5. O quadro de G+7 tem que ficar fora da fusão
static void test_spurious_hold_bench_sequence() {
    const uint32_t G = 100000;
    PulseFilter f(2500);
    SpuriousHold h(10000);
    TEST_ASSERT_TRUE(f.accept(G));
    TEST_ASSERT_FALSE(h.holds(G));
    TEST_ASSERT_TRUE(f.accept(G + 4000));   // falso aceito (lido sem dado novo)
    TEST_ASSERT_FALSE(f.accept(G + 5000));  // o verdadeiro, recusado
    h.noteSpurious(G + 5000);
    TEST_ASSERT_TRUE(f.accept(G + 7000));   // falso aceito: a leitura encontra a amostra de G+5
    TEST_ASSERT_TRUE(h.holds(G + 7000));    // ambíguo
}

// Falso isolado: os quadros até 10 ms depois ficam fora; depois disso, voltam, e a quarentena se encerra
static void test_spurious_hold_isolated() {
    SpuriousHold h(10000);
    h.noteSpurious(1000);
    TEST_ASSERT_TRUE(h.holds(500));     // pulso anterior ao falso, lido depois dele: ambíguo
    TEST_ASSERT_TRUE(h.holds(5000));
    TEST_ASSERT_TRUE(h.holds(10999));
    TEST_ASSERT_FALSE(h.holds(11000));  // 10 ms depois
    TEST_ASSERT_FALSE(h.holds(1000));   // encerrada: não volta a valer (nem com a virada do micros())
}

// Virada do micros() durante a quarentena; reset() a encerra
static void test_spurious_hold_wraparound_and_reset() {
    SpuriousHold h(10000);
    h.noteSpurious(0xFFFFF000u);
    TEST_ASSERT_TRUE(h.holds(0x00001000u));    // 8 192 µs depois, já depois da virada
    TEST_ASSERT_FALSE(h.holds(0x00001710u));   // 10 000 µs depois
    h.noteSpurious(50);
    h.reset();
    TEST_ASSERT_FALSE(h.holds(60));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_windows_continuous_limits);
    RUN_TEST(test_long_stall_closes_several_windows);
    RUN_TEST(test_close_by_clock);
    RUN_TEST(test_pulse_before_window_not_counted);
    RUN_TEST(test_lost_pulses);
    RUN_TEST(test_stale_in_window_of_pulse);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_pulse_at_zero_counts_lost);
    RUN_TEST(test_sample_window_summary);
    RUN_TEST(test_empty_window_summary);
    RUN_TEST(test_check_frame);
    RUN_TEST(test_acq_stats);
    RUN_TEST(test_settle_gate);
    RUN_TEST(test_pulse_filter_accepts_period);
    RUN_TEST(test_pulse_filter_rejects_burst);
    RUN_TEST(test_pulse_filter_isolated_spurious);
    RUN_TEST(test_pulse_filter_boundary);
    RUN_TEST(test_pulse_filter_reset);
    RUN_TEST(test_pulse_filter_wraparound);
    RUN_TEST(test_spurious_hold_bench_sequence);
    RUN_TEST(test_spurious_hold_isolated);
    RUN_TEST(test_spurious_hold_wraparound_and_reset);
    return UNITY_END();
}
