// Testes do estado do link e da contagem de perdas pelo seq (lib/link_stats).
// Rodar no PC: pio test -e native
#include <unity.h>
#include "link_stats.h"

void setUp() {}
void tearDown() {}

static const LinkConfig CFG = {500000, 500, 100, 2000};   // timeout 500 ms, ressinc. > 500, atrasado até 100 / 2 s
static const uint64_t T0 = 10000000;           // 10 s
static const uint64_t PERIOD_US = 20000;       // 50 Hz

// n pacotes em ordem a partir de seq/tMs/us, 20 ms um do outro; devolve o instante do último
static uint64_t feed(LinkStats &s, uint16_t seq, uint32_t tMs, uint64_t us, int n) {
    for (int i = 0; i < n; i++) s.onPacket((uint16_t)(seq + i), tMs + 20u * i, us + PERIOD_US * i);
    return us + PERIOD_US * (n - 1);
}

static void test_first_packet() {
    LinkStats s(CFG);
    TEST_ASSERT_EQUAL(LinkState::NoSignal, s.state());
    TEST_ASSERT_FALSE(s.hasPacket());
    const LinkUpdate u = s.onPacket(100, 5000, T0);
    TEST_ASSERT_TRUE(u.accepted);
    TEST_ASSERT_EQUAL_HEX8(LINK_EV_FIRST, u.events);
    TEST_ASSERT_EQUAL(LinkState::Ok, s.state());
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().received);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
}

static void test_in_order_and_intervals() {
    LinkStats s(CFG);
    feed(s, 0, 1000, T0, 50);
    const LinkWindow w = s.takeWindow();
    TEST_ASSERT_EQUAL_UINT32(50, w.counts.received);
    TEST_ASSERT_EQUAL_UINT32(0, w.counts.lost);
    TEST_ASSERT_EQUAL_UINT32(49, w.intervals);   // o primeiro pacote não tem intervalo
    TEST_ASSERT_EQUAL_UINT32(20000, w.intervalMinUs);
    TEST_ASSERT_EQUAL_UINT32(20000, w.intervalMaxUs);
    TEST_ASSERT_EQUAL_UINT64(49ULL * 20000, w.intervalSumUs);
    TEST_ASSERT_EQUAL_UINT32(20, w.srcIntervalMinMs);
    TEST_ASSERT_EQUAL_UINT32(20, w.srcIntervalMaxMs);
    // A janela foi zerada; os totais, não
    const LinkWindow w2 = s.takeWindow();
    TEST_ASSERT_EQUAL_UINT32(0, w2.counts.received);
    TEST_ASSERT_EQUAL_UINT32(0, w2.intervals);
    TEST_ASSERT_EQUAL_UINT32(0, w2.intervalMinUs);
    TEST_ASSERT_EQUAL_UINT32(0, w2.srcIntervalMinMs);
    TEST_ASSERT_EQUAL_UINT32(0, w2.srcIntervalMaxMs);
    TEST_ASSERT_EQUAL_UINT32(50, s.totals().received);
}

// Lacuna de 3 pacotes: conta 3 perdidos e o intervalo de 80 ms entra no máximo
static void test_gap_counts_lost() {
    LinkStats s(CFG);
    feed(s, 10, 1000, T0, 2);   // 10, 11
    const LinkUpdate u = s.onPacket(15, 1100, T0 + 5 * PERIOD_US);
    TEST_ASSERT_EQUAL_HEX8(0, u.events);
    TEST_ASSERT_EQUAL_UINT32(3, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(3, s.totals().received);
    const LinkWindow w = s.takeWindow();
    TEST_ASSERT_EQUAL_UINT32(20000, w.intervalMinUs);
    TEST_ASSERT_EQUAL_UINT32(80000, w.intervalMaxUs);
}

static void test_duplicate() {
    LinkStats s(CFG);
    feed(s, 7, 1000, T0, 2);   // 7, 8
    const LinkUpdate u = s.onPacket(8, 1020, T0 + 2 * PERIOD_US);
    TEST_ASSERT_EQUAL_HEX8(0, u.events);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().duplicates);
    TEST_ASSERT_EQUAL_UINT32(2, s.totals().received);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT16(8, s.lastSeq());
    // O seguinte continua em ordem
    s.onPacket(9, 1040, T0 + 3 * PERIOD_US);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
}

// Atraso no ar ou no B: as chegadas variam (35 e 5 ms), o t_ms do A não (20 ms)
static void test_src_interval_separates_delay() {
    LinkStats s(CFG);
    s.onPacket(1, 1000, T0);
    s.onPacket(2, 1020, T0 + 35000);
    s.onPacket(3, 1040, T0 + 40000);
    LinkWindow w = s.takeWindow();
    TEST_ASSERT_EQUAL_UINT32(5000, w.intervalMinUs);
    TEST_ASSERT_EQUAL_UINT32(35000, w.intervalMaxUs);
    TEST_ASSERT_EQUAL_UINT32(20, w.srcIntervalMinMs);
    TEST_ASSERT_EQUAL_UINT32(20, w.srcIntervalMaxMs);
    // Atraso no envio do A: o t_ms varia junto (33 e 7 ms)
    s.onPacket(4, 1073, T0 + 73000);
    s.onPacket(5, 1080, T0 + 80000);
    w = s.takeWindow();
    TEST_ASSERT_EQUAL_UINT32(7, w.srcIntervalMinMs);
    TEST_ASSERT_EQUAL_UINT32(33, w.srcIntervalMaxMs);
}

// Volta do seq de 65535 para 0, com e sem perda no meio
static void test_seq_wrap() {
    LinkStats s(CFG);
    feed(s, 65534, 1000, T0, 3);   // 65534, 65535, 0
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().resyncs);
    TEST_ASSERT_EQUAL_UINT16(0, s.lastSeq());

    LinkStats s2(CFG);
    s2.onPacket(65534, 1000, T0);
    s2.onPacket(1, 1060, T0 + 3 * PERIOD_US);   // perdeu 65535 e 0
    TEST_ASSERT_EQUAL_UINT32(2, s2.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(0, s2.totals().resyncs);
}

// A reiniciado: seq e t_ms voltam a 0; ressincroniza sem milhares de perdas falsas
static void test_restart_resyncs() {
    LinkStats s(CFG);
    const uint64_t last = feed(s, 3000, 60000, T0, 5);
    const LinkUpdate u = s.onPacket(0, 1000, last + 1500000);
    TEST_ASSERT_TRUE(u.events & LINK_EV_RESYNC);
    TEST_ASSERT_EQUAL_UINT16(3004, u.seqFrom);
    TEST_ASSERT_EQUAL_UINT16(0, u.seqTo);
    TEST_ASSERT_EQUAL_UINT32(60080, u.tFrom);
    TEST_ASSERT_EQUAL_UINT32(1000, u.tTo);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().resyncs);
    TEST_ASSERT_EQUAL_UINT32(6, s.totals().received);
    // Depois, conta normalmente
    s.onPacket(2, 1040, last + 1500000 + 2 * PERIOD_US);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().lost);
}

// A reiniciado com o seq anterior baixo: o salto do seq não basta (d pequeno), o t_ms voltando sim
static void test_restart_detected_by_tms() {
    LinkStats s(CFG);
    feed(s, 0, 5000, T0, 3);   // 0, 1, 2 com t_ms ~5 s
    const LinkUpdate u = s.onPacket(3, 1200, T0 + 3 * PERIOD_US);
    TEST_ASSERT_TRUE(u.events & LINK_EV_RESYNC);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().resyncs);
    // Um pacote com o mesmo seq e t_ms menor também não é duplicado: é reinício
    LinkStats s2(CFG);
    feed(s2, 0, 5000, T0, 2);   // 0, 1
    const LinkUpdate u2 = s2.onPacket(1, 1200, T0 + 2 * PERIOD_US);
    TEST_ASSERT_TRUE(u2.events & LINK_EV_RESYNC);
    TEST_ASSERT_EQUAL_UINT32(0, s2.totals().duplicates);
}

// Salto acima do limite (ou seq para trás) ressincroniza; no limite, conta como perda
static void test_resync_gap_limit() {
    LinkStats s(CFG);
    s.onPacket(100, 1000, T0);
    s.onPacket(600, 2000, T0 + PERIOD_US);   // d = 500: perdas
    TEST_ASSERT_EQUAL_UINT32(499, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().resyncs);
    s.onPacket(1101, 3000, T0 + 2 * PERIOD_US);   // d = 501: ressincroniza
    TEST_ASSERT_EQUAL_UINT32(499, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().resyncs);
    s.onPacket(1090, 3100, T0 + 3 * PERIOD_US);   // para trás, com t_ms crescente
    TEST_ASSERT_EQUAL_UINT32(2, s.totals().resyncs);
}

// Timeout e restabelecimento, com as perdas da falta contadas pelo seq
static void test_timeout_and_restore() {
    LinkStats s(CFG);
    const uint64_t last = feed(s, 0, 1000, T0, 10);   // seq 0..9
    TEST_ASSERT_EQUAL_HEX8(0, s.poll(last + 500000).events);   // no limite: ainda OK
    TEST_ASSERT_EQUAL(LinkState::Ok, s.state());
    const LinkUpdate lost = s.poll(last + 500001);
    TEST_ASSERT_EQUAL_HEX8(LINK_EV_LOST, lost.events);
    TEST_ASSERT_EQUAL_UINT64(500001, lost.outageUs);
    TEST_ASSERT_EQUAL(LinkState::Lost, s.state());
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().timeouts);
    TEST_ASSERT_EQUAL_HEX8(0, s.poll(last + 900000).events);   // conta uma vez só
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().timeouts);

    s.takeWindow();
    const LinkUpdate back = s.onPacket(60, 2000, last + 1000000);   // 50 pacotes perdidos (10..59)
    TEST_ASSERT_EQUAL_HEX8(LINK_EV_RESTORED, back.events);
    TEST_ASSERT_EQUAL_UINT64(1000000, back.outageUs);
    TEST_ASSERT_EQUAL(LinkState::Ok, s.state());
    TEST_ASSERT_EQUAL_UINT32(50, s.totals().lost);
    // O intervalo da falta não entra no jitter
    TEST_ASSERT_EQUAL_UINT32(0, s.takeWindow().intervals);
}

// Falta longa com o A reiniciado: restabelecido e ressincronizado no mesmo pacote
static void test_restore_with_resync() {
    LinkStats s(CFG);
    const uint64_t last = feed(s, 2000, 40000, T0, 3);
    s.poll(last + 600000);
    const LinkUpdate u = s.onPacket(0, 1100, last + 3000000);
    TEST_ASSERT_EQUAL_HEX8(LINK_EV_RESTORED | LINK_EV_RESYNC, u.events);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
}

// Sem pacote nenhum, o poll não muda o estado
static void test_poll_without_signal() {
    LinkStats s(CFG);
    TEST_ASSERT_EQUAL_HEX8(0, s.poll(T0 + 10000000).events);
    TEST_ASSERT_EQUAL(LinkState::NoSignal, s.state());
}

// Chegada carimbada um pouco depois do instante do poll (outra task): não vira timeout
static void test_poll_arrival_after_now() {
    LinkStats s(CFG);
    s.onPacket(1, 1000, T0 + 100);
    TEST_ASSERT_EQUAL_HEX8(0, s.poll(T0).events);
    TEST_ASSERT_EQUAL(LinkState::Ok, s.state());
}

// Pacote atrasado (100 → 101 → 100 velho → 102): descartado, sem ressincronização nem perda falsa
static void test_late_packet_discarded() {
    LinkStats s(CFG);
    s.onPacket(100, 5000, T0);
    s.onPacket(101, 5020, T0 + PERIOD_US);
    const LinkUpdate u = s.onPacket(100, 5000, T0 + 2 * PERIOD_US);
    TEST_ASSERT_FALSE(u.accepted);
    TEST_ASSERT_EQUAL_HEX8(0, u.events);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().late);
    TEST_ASSERT_EQUAL_UINT16(101, s.lastSeq());
    TEST_ASSERT_TRUE(s.onPacket(102, 5040, T0 + 3 * PERIOD_US).accepted);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().resyncs);
    TEST_ASSERT_EQUAL_UINT32(3, s.totals().received);
}

// Um pacote dado como perdido que chega tarde continua contado como perdido, e é descartado
static void test_late_after_counted_lost() {
    LinkStats s(CFG);
    s.onPacket(10, 1000, T0);
    s.onPacket(12, 1040, T0 + PERIOD_US);   // 11 perdido
    TEST_ASSERT_FALSE(s.onPacket(11, 1020, T0 + 2 * PERIOD_US).accepted);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().lost);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().late);
}

// Fora da janela de atraso (seq 101 atrás ou t_ms 2,001 s atrás): reinício, ressincroniza
static void test_late_window_limits() {
    LinkStats s(CFG);
    s.onPacket(1000, 60000, T0);
    TEST_ASSERT_FALSE(s.onPacket(900, 58000, T0 + PERIOD_US).accepted);   // 100 atrás e 2 s: atrasado
    const LinkUpdate a = s.onPacket(899, 58000, T0 + 2 * PERIOD_US);     // 101 atrás
    TEST_ASSERT_TRUE(a.accepted);
    TEST_ASSERT_TRUE(a.events & LINK_EV_RESYNC);

    LinkStats s2(CFG);
    s2.onPacket(1000, 60000, T0);
    const LinkUpdate b = s2.onPacket(999, 57999, T0 + PERIOD_US);   // 1 atrás, mas 2,001 s
    TEST_ASSERT_TRUE(b.events & LINK_EV_RESYNC);
}

// Caso ambíguo: o A reinicia com poucos pacotes enviados. Os primeiros parecem atrasados e são descartados;
// em até lateMaxPkts pacotes, o fluxo novo volta a ser aceito (em ordem ou com ressincronização), sem perda falsa
static void test_quick_restart_recovers() {
    LinkStats s(CFG);
    feed(s, 0, 1100, T0, 50);   // seq 0..49, t_ms 1100..2080
    const uint64_t us = T0 + 60 * PERIOD_US;
    int firstAccepted = -1;
    for (int i = 0; i < 100 && firstAccepted < 0; i++) {   // A reiniciado: seq 0.., t_ms 1107.. (outra fase)
        if (s.onPacket((uint16_t)i, 1107u + 20u * i, us + PERIOD_US * i).accepted) firstAccepted = i;
    }
    TEST_ASSERT_TRUE(firstAccepted >= 0 && firstAccepted <= 50);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
    // Os seguintes, em ordem
    const uint32_t before = s.totals().received;
    feed(s, (uint16_t)(firstAccepted + 1), 1107u + 20u * (firstAccepted + 1), us + PERIOD_US * (firstAccepted + 1), 10);
    TEST_ASSERT_EQUAL_UINT32(before + 10, s.totals().received);
    TEST_ASSERT_EQUAL_UINT32(0, s.totals().lost);
}

// Duplicado não é aceito (não vai aos consumidores)
static void test_duplicate_not_accepted() {
    LinkStats s(CFG);
    TEST_ASSERT_TRUE(s.onPacket(5, 1000, T0).accepted);
    TEST_ASSERT_FALSE(s.onPacket(5, 1000, T0 + 1000).accepted);
}

// Chegadas separadas por mais que o timeout, sem poll no meio (task atrasada): queda registrada no pacote
static void test_gap_without_poll_counts_timeout() {
    LinkStats s(CFG);
    s.onPacket(1, 1000, T0);
    const LinkUpdate u = s.onPacket(31, 1600, T0 + 600000);   // 600 ms depois
    TEST_ASSERT_TRUE(u.accepted);
    TEST_ASSERT_EQUAL_HEX8(LINK_EV_LOST | LINK_EV_RESTORED, u.events);
    TEST_ASSERT_EQUAL_UINT64(600000, u.outageUs);
    TEST_ASSERT_EQUAL_UINT32(1, s.totals().timeouts);
    TEST_ASSERT_EQUAL_UINT32(29, s.totals().lost);
    TEST_ASSERT_EQUAL(LinkState::Ok, s.state());
    TEST_ASSERT_EQUAL_UINT32(0, s.takeWindow().intervals);   // a falta não entra no jitter
    // No limite (500 ms), não
    LinkStats s2(CFG);
    s2.onPacket(1, 1000, T0);
    TEST_ASSERT_EQUAL_HEX8(0, s2.onPacket(26, 1500, T0 + 500000).events);
}

static void test_state_text() {
    TEST_ASSERT_EQUAL_STRING("sem sinal", linkStateText(LinkState::NoSignal));
    TEST_ASSERT_EQUAL_STRING("OK", linkStateText(LinkState::Ok));
    TEST_ASSERT_EQUAL_STRING("PERDIDO", linkStateText(LinkState::Lost));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_packet);
    RUN_TEST(test_in_order_and_intervals);
    RUN_TEST(test_gap_counts_lost);
    RUN_TEST(test_duplicate);
    RUN_TEST(test_src_interval_separates_delay);
    RUN_TEST(test_seq_wrap);
    RUN_TEST(test_restart_resyncs);
    RUN_TEST(test_restart_detected_by_tms);
    RUN_TEST(test_resync_gap_limit);
    RUN_TEST(test_timeout_and_restore);
    RUN_TEST(test_restore_with_resync);
    RUN_TEST(test_poll_without_signal);
    RUN_TEST(test_poll_arrival_after_now);
    RUN_TEST(test_late_packet_discarded);
    RUN_TEST(test_late_after_counted_lost);
    RUN_TEST(test_late_window_limits);
    RUN_TEST(test_quick_restart_recovers);
    RUN_TEST(test_duplicate_not_accepted);
    RUN_TEST(test_gap_without_poll_counts_timeout);
    RUN_TEST(test_state_text);
    return UNITY_END();
}
