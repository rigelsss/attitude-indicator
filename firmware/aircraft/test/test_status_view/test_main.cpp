// Testes da tela do OLED de status e do padrão do LED (lib/status_view).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "status_view.h"

void setUp() {}
void tearDown() {}

static const StatusViewParams P = {2000, 10000, 500, 1000, 100, 2000};

// Tudo bem, rádio ativo, sem avisos
static StatusInputs goodInputs() {
    StatusInputs in;
    memset(&in, 0, sizeof(in));
    in.haveState = true;
    in.mpuOk = in.baroOk = in.magOk = true;
    in.radio = RadioState::Active;
    in.uptimeS = 5025;   // 1:23:45
    in.lastReset = ResetKind::Normal;
    return in;
}

static StatusScreen show(const StatusView &v, uint32_t nowMs) {
    StatusScreen s;
    v.render(nowMs, s);
    return s;
}

// Colunas [from, from + n) em vídeo inverso
static uint32_t bits(int from, int n) {
    uint32_t m = 0;
    for (int c = from; c < from + n; c++) m |= 1UL << c;
    return m;
}

// ---------- Utilitários ----------

static void test_utf8_columns() {
    TEST_ASSERT_EQUAL_UINT32(0, utf8Columns(""));
    TEST_ASSERT_EQUAL_UINT32(10, utf8Columns("CALIBRANDO"));
    TEST_ASSERT_EQUAL_UINT32(18, utf8Columns("REINÍCIO: BROWNOUT"));   // Í = 2 bytes, 1 coluna
    TEST_ASSERT_EQUAL_UINT32(6, utf8Columns("PÂNICO"));
}

static void test_uptime_format() {
    char b[16];
    formatUptime(0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("0:00:00", b);
    formatUptime(59, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("0:00:59", b);
    formatUptime(3600, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1:00:00", b);
    formatUptime(5025, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1:23:45", b);
    formatUptime(100 * 3600 + 61, b, sizeof(b));   // horas sem limite
    TEST_ASSERT_EQUAL_STRING("100:01:01", b);
}

// ---------- L1: sensores ----------

// As 8 combinações; o "--" de cada sensor falhando em vídeo inverso (colunas 4, 13 e 21)
static void test_sensor_line_all_combinations() {
    for (int m = 0; m < 8; m++) {
        StatusInputs in = goodInputs();
        in.mpuOk = (m & 1) != 0;
        in.baroOk = (m & 2) != 0;
        in.magOk = (m & 4) != 0;
        StatusView v(P);
        v.update(in, 0);
        const StatusScreen s = show(v, 0);
        char expect[STATUS_LINE_BYTES];
        snprintf(expect, sizeof(expect), "MPU %s  BARO %s  MAG %s", in.mpuOk ? "OK" : "--", in.baroOk ? "OK" : "--",
                 in.magOk ? "OK" : "--");
        TEST_ASSERT_EQUAL_STRING(expect, s.line[0]);
        uint32_t inv = 0;
        if (!in.mpuOk) inv |= bits(4, 2);
        if (!in.baroOk) inv |= bits(13, 2);
        if (!in.magOk) inv |= bits(21, 2);
        TEST_ASSERT_EQUAL_HEX32(inv, s.inverse[0]);
    }
}

static void test_sensor_line_before_state() {
    StatusInputs in = goodInputs();
    in.haveState = false;
    in.calibrating = true;   // sem estado, sem o aviso de calibração
    StatusView v(P);
    v.update(in, 0);
    const StatusScreen s = show(v, 0);
    TEST_ASSERT_EQUAL_STRING("aguardando sensores", s.line[0]);
    TEST_ASSERT_EQUAL_HEX32(0, s.inverse[0]);
    TEST_ASSERT_EQUAL_STRING("SEM AVISOS", s.line[3]);
}

// ---------- L2: link ----------

static void test_link_line_states() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    in.radio = RadioState::Off;
    v.update(in, 0);
    TEST_ASSERT_EQUAL_STRING("RÁDIO DESLIGADO", show(v, 0).line[1]);
    in.radio = RadioState::Starting;
    v.update(in, 0);
    TEST_ASSERT_EQUAL_STRING("RÁDIO LIGANDO...", show(v, 0).line[1]);
    in.radio = RadioState::Active;
    v.update(in, 0);   // primeiro segundo: sem taxa ainda
    TEST_ASSERT_EQUAL_STRING("TX --/s  ENTREGUES --/s", show(v, 0).line[1]);
}

// Taxas pela diferença dos contadores a cada segundo, com voltas de 50 ms como a da status_task
static void test_link_rates() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    uint32_t t = 0;
    in.sent = 1000;
    in.delivered = 900;
    for (; t < 1000; t += 50) {
        v.update(in, t);
        in.sent += 2;   // 40 por segundo
    }
    TEST_ASSERT_FALSE(v.haveRates());
    v.update(in, t);   // 1 s depois da referência
    TEST_ASSERT_TRUE(v.haveRates());
    TEST_ASSERT_EQUAL_UINT32(40, v.sentPerS());
    TEST_ASSERT_EQUAL_UINT32(0, v.deliveredPerS());
    TEST_ASSERT_EQUAL_STRING("TX 40/s  ENTREGUES 0/s", show(v, t).line[1]);
}

// A subtração sem sinal atravessa a virada dos contadores de 32 bits
static void test_link_rates_wrap() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    in.sent = 0xFFFFFFF0u;
    in.delivered = 0xFFFFFFF0u;
    v.update(in, 0);
    in.sent += 50;
    in.delivered += 50;
    v.update(in, 1000);
    TEST_ASSERT_EQUAL_UINT32(50, v.sentPerS());
    TEST_ASSERT_EQUAL_UINT32(50, v.deliveredPerS());
    TEST_ASSERT_EQUAL_STRING("TX 50/s  ENTREGUES 50/s", show(v, 1000).line[1]);
}

// Volta atrasada (1,2 s): a taxa é por segundo, não por janela
static void test_link_rates_scaled() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    in.sent = 60;
    v.update(in, 1200);
    TEST_ASSERT_EQUAL_UINT32(50, v.sentPerS());
}

// Acima de 2 dígitos, a taxa satura em 99 (a linha cabe nos 128 px)
static void test_link_rates_clamped() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    in.sent = in.delivered = 5000;
    v.update(in, 1000);
    TEST_ASSERT_EQUAL_STRING("TX 99/s  ENTREGUES 99/s", show(v, 1000).line[1]);
}

// O rádio deixando de estar ativo zera a referência: ao voltar, recomeça com "--"
static void test_link_rates_reset_when_inactive() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    in.sent = 50;
    v.update(in, 1000);
    TEST_ASSERT_TRUE(v.haveRates());
    in.radio = RadioState::Off;
    v.update(in, 1050);
    TEST_ASSERT_FALSE(v.haveRates());
    in.radio = RadioState::Active;
    v.update(in, 1100);
    TEST_ASSERT_EQUAL_STRING("TX --/s  ENTREGUES --/s", show(v, 1100).line[1]);
}

// ---------- L3: tempo ligado ----------

static void test_uptime_line() {
    StatusView v(P);
    v.update(goodInputs(), 0);
    const StatusScreen s = show(v, 0);
    TEST_ASSERT_EQUAL_STRING("LIGADO 1:23:45", s.line[2]);
    TEST_ASSERT_EQUAL_HEX32(0, s.inverse[2]);
}

// ---------- L4: avisos ----------

static void test_no_warnings() {
    StatusView v(P);
    v.update(goodInputs(), 0);
    const StatusScreen s = show(v, 0);
    TEST_ASSERT_EQUAL_STRING("SEM AVISOS", s.line[3]);
    TEST_ASSERT_EQUAL_HEX32(0, s.inverse[3]);
}

static void test_single_warnings() {
    struct Case {
        ResetKind   reset;
        const char *text;
    };
    const Case cases[] = {{ResetKind::Brownout, "REINÍCIO: BROWNOUT"},
                          {ResetKind::Panic, "REINÍCIO: PÂNICO"},
                          {ResetKind::Watchdog, "REINÍCIO: WATCHDOG"}};
    for (const Case &c : cases) {
        StatusInputs in = goodInputs();
        in.lastReset = c.reset;
        StatusView v(P);
        v.update(in, 0);
        const StatusScreen s = show(v, 0);
        TEST_ASSERT_EQUAL_STRING(c.text, s.line[3]);
        TEST_ASSERT_EQUAL_HEX32(bits(0, (int)utf8Columns(c.text)), s.inverse[3]);
    }
    StatusInputs in = goodInputs();
    in.calibrating = true;
    StatusView v(P);
    v.update(in, 0);
    const StatusScreen s = show(v, 0);
    TEST_ASSERT_EQUAL_STRING("CALIBRANDO", s.line[3]);
    TEST_ASSERT_EQUAL_HEX32(bits(0, 10), s.inverse[3]);
}

// Os pulsos falsos aparecem quando o total sobe e somem spuriousShowMs depois do último aumento
static void test_spurious_window() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    TEST_ASSERT_FALSE(v.spuriousRecent());   // total 0 desde o boot: sem aviso
    in.intSpurious = 37;
    v.update(in, 1000);
    TEST_ASSERT_TRUE(v.spuriousRecent());
    TEST_ASSERT_EQUAL_STRING("INT: 37 PULSOS FALSOS", show(v, 1000).line[3]);
    v.update(in, 10999);
    TEST_ASSERT_TRUE(v.spuriousRecent());
    v.update(in, 11000);
    TEST_ASSERT_FALSE(v.spuriousRecent());
    TEST_ASSERT_EQUAL_STRING("SEM AVISOS", show(v, 11000).line[3]);
    in.intSpurious = 40;   // novo aumento: volta, com o total desde o boot
    v.update(in, 20000);
    TEST_ASSERT_EQUAL_STRING("INT: 40 PULSOS FALSOS", show(v, 20000).line[3]);
}

static void test_spurious_text_clamped() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    in.intSpurious = 9999;
    v.update(in, 1);
    TEST_ASSERT_EQUAL_STRING("INT: 9999 PULSOS FALSOS", show(v, 1).line[3]);
    in.intSpurious = 123456;
    v.update(in, 2);
    TEST_ASSERT_EQUAL_STRING("INT: >9999 PULSOS FALSOS", show(v, 2).line[3]);
}

// Vários avisos: prioridade calibração → reinício → pulsos falsos, um por vez a cada warnRotateMs
static void test_warning_rotation() {
    StatusInputs in = goodInputs();
    in.calibrating = true;
    in.lastReset = ResetKind::Brownout;
    StatusView v(P);
    v.update(in, 0);
    in.intSpurious = 5;
    v.update(in, 100);
    TEST_ASSERT_EQUAL_STRING("CALIBRANDO", show(v, 100).line[3]);
    TEST_ASSERT_EQUAL_STRING("REINÍCIO: BROWNOUT", show(v, 2000).line[3]);
    TEST_ASSERT_EQUAL_STRING("INT: 5 PULSOS FALSOS", show(v, 4000).line[3]);
    TEST_ASSERT_EQUAL_STRING("CALIBRANDO", show(v, 6000).line[3]);
    // Dois avisos: alternam
    in.calibrating = false;
    v.update(in, 6100);
    TEST_ASSERT_EQUAL_STRING("INT: 5 PULSOS FALSOS", show(v, 6100).line[3]);
    TEST_ASSERT_EQUAL_STRING("REINÍCIO: BROWNOUT", show(v, 8000).line[3]);
}

// Todas as combinações de flags: nenhuma linha passa de 25 colunas, nem a máscara do inverso
static void test_every_combination_fits() {
    const ResetKind resets[] = {ResetKind::Normal, ResetKind::Brownout, ResetKind::Panic, ResetKind::Watchdog};
    const RadioState radios[] = {RadioState::Off, RadioState::Starting, RadioState::Active};
    for (int m = 0; m < 64; m++)
        for (ResetKind r : resets)
            for (RadioState rs : radios) {
                StatusInputs in = goodInputs();
                in.haveState = (m & 1) != 0;
                in.mpuOk = (m & 2) != 0;
                in.baroOk = (m & 4) != 0;
                in.magOk = (m & 8) != 0;
                in.calibrating = (m & 16) != 0;
                in.lastReset = r;
                in.radio = rs;
                in.uptimeS = 0xFFFFFFFFu;   // o maior tempo ligado: 1193046:28:15
                StatusView v(P);
                v.update(in, 0);
                in.sent = in.delivered = 0xFFFFFFFFu;
                in.intSpurious = (m & 32) ? 0xFFFFFFFFu : 0;
                v.update(in, 1000);
                for (uint32_t t = 1000; t < 1000 + 3 * P.warnRotateMs; t += P.warnRotateMs) {
                    const StatusScreen s = show(v, t);
                    for (int l = 0; l < STATUS_LINES; l++) {
                        TEST_ASSERT_LESS_OR_EQUAL_UINT32(STATUS_COLS, utf8Columns(s.line[l]));
                        TEST_ASSERT_EQUAL_HEX32(0, s.inverse[l] >> STATUS_COLS);
                    }
                }
            }
}

// ---------- LED ----------

static void test_led_modes() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    in.radio = RadioState::Starting;
    v.update(in, 0);
    TEST_ASSERT_EQUAL_INT((int)LedMode::Off, (int)v.ledMode());
    TEST_ASSERT_FALSE(v.ledOn(0));
    in.radio = RadioState::Active;
    v.update(in, 0);
    TEST_ASSERT_EQUAL_INT((int)LedMode::NoDelivery, (int)v.ledMode());   // primeiro segundo: sem taxa
    in.sent = 40;
    v.update(in, 1000);
    TEST_ASSERT_EQUAL_INT((int)LedMode::NoDelivery, (int)v.ledMode());   // enviando, sem entregas (sem o B)
    in.sent = 90;
    in.delivered = 50;
    v.update(in, 2000);
    TEST_ASSERT_EQUAL_INT((int)LedMode::Delivering, (int)v.ledMode());
}

// Delivering: 500 ms aceso a cada 1 s; NoDelivery: 100 ms aceso a cada 2 s
static void test_led_phases() {
    StatusInputs in = goodInputs();
    StatusView v(P);
    v.update(in, 0);
    TEST_ASSERT_TRUE(v.ledOn(0));
    TEST_ASSERT_TRUE(v.ledOn(50));
    TEST_ASSERT_FALSE(v.ledOn(100));
    TEST_ASSERT_FALSE(v.ledOn(1950));
    TEST_ASSERT_TRUE(v.ledOn(2000));
    in.sent = in.delivered = 50;
    v.update(in, 1000);
    TEST_ASSERT_TRUE(v.ledOn(1000));
    TEST_ASSERT_TRUE(v.ledOn(1450));
    TEST_ASSERT_FALSE(v.ledOn(1500));
    TEST_ASSERT_FALSE(v.ledOn(1950));
    TEST_ASSERT_TRUE(v.ledOn(2000));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_utf8_columns);
    RUN_TEST(test_uptime_format);
    RUN_TEST(test_sensor_line_all_combinations);
    RUN_TEST(test_sensor_line_before_state);
    RUN_TEST(test_link_line_states);
    RUN_TEST(test_link_rates);
    RUN_TEST(test_link_rates_wrap);
    RUN_TEST(test_link_rates_scaled);
    RUN_TEST(test_link_rates_clamped);
    RUN_TEST(test_link_rates_reset_when_inactive);
    RUN_TEST(test_uptime_line);
    RUN_TEST(test_no_warnings);
    RUN_TEST(test_single_warnings);
    RUN_TEST(test_spurious_window);
    RUN_TEST(test_spurious_text_clamped);
    RUN_TEST(test_warning_rotation);
    RUN_TEST(test_every_combination_fits);
    RUN_TEST(test_led_modes);
    RUN_TEST(test_led_phases);
    return UNITY_END();
}
