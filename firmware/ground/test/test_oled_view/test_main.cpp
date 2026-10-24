// Testes do texto das páginas do OLED (lib/oled_view).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <math.h>
#include <string.h>
#include "alarm_logic.h"
#include "oled_view.h"

void setUp() {}
void tearDown() {}

static OledInputs base() {
    OledInputs in;
    memset(&in, 0, sizeof(in));
    in.everLinked = true;
    in.linkOk = true;
    in.pkt.roll = 12.34f;
    in.pkt.pitch = -4.5f;
    in.pkt.heading = 95.4f;
    in.pkt.gz = 3.21f;
    in.pkt.altitude_m = 0.25f;
    in.pkt.pressure_hpa = 1009.31f;
    in.pkt.temp_c = 25.94f;
    in.pkt.status = ST_MPU_OK | ST_BARO_OK | ST_MAG_OK | ST_GYRO_CAL | ST_MAG_CAL;
    in.pkt.valid = VAL_ATT | VAL_ALT | VAL_HDG;
    in.pkt.loop_us = 180;
    in.link.valid = true;
    in.link.recPerS = 50;
    in.link.lostTotal = 1;
    in.link.lossPct = 0.02f;
    in.link.timeouts = 0;
    in.link.haveIntervals = true;
    in.link.intMinMs = 12.3f;
    in.link.intAvgMs = 20.0f;
    in.link.intMaxMs = 28.4f;
    in.alarmText = alarmKindShort(AlarmKind::None);
    in.page = 0;
    in.pages = 2;
    in.headingTrue = false;
    in.declinationDeg = -20.7f;
    return in;
}

static OledPage build(const OledInputs &in) {
    OledPage p;
    oledBuildPage(in, p);
    return p;
}

static void test_data_page() {
    const OledPage p = build(base());
    TEST_ASSERT_FALSE(p.banner);
    TEST_ASSERT_EQUAL_STRING("DADOS             1/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("ROLL +12.3 PIT  -4.5", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("PROA 095 MAG", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("ALT  +0.25 m", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("1009.3hPa  25.9C", p.lines[4]);
    TEST_ASSERT_EQUAL_STRING("CURVA   +3.2 g/s", p.lines[5]);
}

static void test_link_page() {
    OledInputs in = base();
    in.page = 1;
    const OledPage p = build(in);
    TEST_ASSERT_FALSE(p.banner);
    TEST_ASSERT_EQUAL_STRING("LINK OK 50/s      2/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("perd 1 0.02% q0", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("int 12/20/28 at 0", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("MPU ok BAR ok MAG ok", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("cal: - loop 180us", p.lines[4]);
    TEST_ASSERT_EQUAL_STRING("ALARME nenhum", p.lines[5]);
}

static void test_no_signal() {
    OledInputs in = base();
    in.everLinked = false;
    in.linkOk = false;
    in.link.valid = false;
    OledPage p = build(in);
    TEST_ASSERT_TRUE(p.banner);
    TEST_ASSERT_EQUAL_STRING("SEM SINAL         1/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("ROLL   --- PIT   ---", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("PROA --- MAG", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("ALT  --- m", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("---hPa  ---C", p.lines[4]);
    TEST_ASSERT_EQUAL_STRING("CURVA    --- g/s", p.lines[5]);
    in.page = 1;
    p = build(in);
    TEST_ASSERT_TRUE(p.banner);
    TEST_ASSERT_EQUAL_STRING("SEM SINAL         2/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("perd ---", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("int --- at ---", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("MPU ? BAR ? MAG ?", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("cal: ---", p.lines[4]);
}

// Link perdido: faixa e "---" na página 1 (o último valor não fica congelado); a página 2 mantém o histórico
static void test_link_lost() {
    OledInputs in = base();
    in.linkOk = false;
    OledPage p = build(in);
    TEST_ASSERT_TRUE(p.banner);
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO      1/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("ROLL   --- PIT   ---", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("PROA --- MAG", p.lines[2]);
    in.page = 1;
    p = build(in);
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO      2/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("perd 1 0.02% q0", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("MPU ok BAR ok MAG ok", p.lines[3]);   // do último pacote
}

// Dado atrasado: faixa própria e "---" na página 1; o atraso aparece na página 2
// Atraso sempre inteiro: com três intervalos de três dígitos, o intervalo vira mín-máx (caso da revisão)
static void test_delay_never_cut() {
    OledInputs in = base();
    in.page = 1;
    in.link.intMinMs = 100.0f;
    in.link.intAvgMs = 200.0f;
    in.link.intMaxMs = 400.0f;
    in.delayMs = 412;
    TEST_ASSERT_EQUAL_STRING("int 100-400 at 412", build(in).lines[2]);   // "int 100/200/400 at 412" teria 22
    in.delayMs = 41;
    TEST_ASSERT_EQUAL_STRING("int 100/200/400 at 41", build(in).lines[2]);   // 21: cabe completo
    // Pior caso: tudo limitado a 9999
    in.link.intMinMs = 12000.0f;
    in.link.intAvgMs = 15000.0f;
    in.link.intMaxMs = 20000.0f;
    in.delayMs = 50000;
    TEST_ASSERT_EQUAL_STRING("int 9999-9999 at 9999", build(in).lines[2]);
}

static void test_data_late() {
    OledInputs in = base();
    in.dataLate = true;
    in.delayMs = 412;
    OledPage p = build(in);
    TEST_ASSERT_TRUE(p.banner);
    TEST_ASSERT_EQUAL_STRING("DADO ATRASADO     1/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("ROLL   --- PIT   ---", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("PROA --- MAG", p.lines[2]);
    in.page = 1;
    p = build(in);
    TEST_ASSERT_TRUE(p.banner);
    TEST_ASSERT_EQUAL_STRING("DADO ATRASADO     2/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("int 12/20/28 at 412", p.lines[2]);
    // Link perdido tem prioridade no título
    in.linkOk = false;
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO      2/2", build(in).lines[0]);
}

static void test_invalid_flags() {
    OledInputs in = base();
    in.pkt.valid = 0;
    OledPage p = build(in);
    TEST_ASSERT_FALSE(p.banner);
    TEST_ASSERT_EQUAL_STRING("ROLL   --- PIT   ---", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("PROA --- MAG", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("ALT  --- m", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("---hPa  ---C", p.lines[4]);   // sem VAL_ALT, a leitura pode estar parada
    TEST_ASSERT_EQUAL_STRING("CURVA    --- g/s", p.lines[5]);
}

// Barômetro respondendo (ST_BARO_OK) com a leitura velha (VAL_ALT desligada): pressão e temperatura somem
static void test_pressure_needs_val_alt() {
    OledInputs in = base();
    in.pkt.valid &= ~VAL_ALT;
    TEST_ASSERT_EQUAL_STRING("---hPa  ---C", build(in).lines[4]);
    in = base();
    in.pkt.status &= ~ST_BARO_OK;   // a VAL_ALT já exige o barômetro ok no A; aqui vale só a flag
    TEST_ASSERT_EQUAL_STRING("1009.3hPa  25.9C", build(in).lines[4]);
}

// Atitude degradada: o título avisa; os valores continuam
static void test_attitude_degraded() {
    OledInputs in = base();
    in.pkt.valid |= VAL_ATT_DEGRADED;
    OledPage p = build(in);
    TEST_ASSERT_FALSE(p.banner);
    TEST_ASSERT_EQUAL_STRING("ATT DEGRADADA     1/2", p.lines[0]);
    TEST_ASSERT_EQUAL_STRING("ROLL +12.3 PIT  -4.5", p.lines[1]);
    // Sem link, a faixa tem prioridade
    in.linkOk = false;
    p = build(in);
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO      1/2", p.lines[0]);
}

// VAL_ATT ligada com ângulo não finito: "---", como no alarme
static void test_non_finite_attitude() {
    OledInputs in = base();
    in.pkt.roll = NAN;
    const OledPage p = build(in);
    TEST_ASSERT_EQUAL_STRING("ROLL   --- PIT   ---", p.lines[1]);
}

static void test_heading_gyro_only_and_wrap() {
    OledInputs in = base();
    in.pkt.valid |= VAL_HDG_GYRO_ONLY;
    TEST_ASSERT_EQUAL_STRING("PROA 095~MAG", build(in).lines[2]);
    in.pkt.valid &= ~VAL_HDG_GYRO_ONLY;
    in.pkt.heading = 359.6f;
    TEST_ASSERT_EQUAL_STRING("PROA 000 MAG", build(in).lines[2]);   // e não 360
    in.pkt.heading = 3.2f;
    TEST_ASSERT_EQUAL_STRING("PROA 003 MAG", build(in).lines[2]);
}

// Verdadeira: magnética + declinação (−20,7°), dando a volta
static void test_heading_true() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 74.7f, oledHeading(95.4f, true, -20.7f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 349.3f, oledHeading(10.0f, true, -20.7f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, oledHeading(10.0f, false, -20.7f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, oledHeading(350.0f, true, 15.0f));
    OledInputs in = base();
    in.headingTrue = true;
    TEST_ASSERT_EQUAL_STRING("PROA 075 VERD", build(in).lines[2]);
}

// Sem "-0.0" e sinal nos negativos
static void test_signs_and_rounding() {
    OledInputs in = base();
    in.pkt.roll = -0.04f;
    in.pkt.pitch = -179.94f;
    in.pkt.altitude_m = -0.004f;
    in.pkt.gz = -0.04f;
    const OledPage p = build(in);
    TEST_ASSERT_EQUAL_STRING("ROLL  +0.0 PIT-179.9", p.lines[1]);
    TEST_ASSERT_EQUAL_STRING("ALT  +0.00 m", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("CURVA   +0.0 g/s", p.lines[5]);
}

static void test_link_page_states() {
    OledInputs in = base();
    in.page = 1;
    in.pkt.status |= ST_CALIBRATING;
    in.pkt.status &= ~ST_MAG_OK;
    in.link.haveIntervals = false;
    in.alarmText = alarmKindShort(AlarmKind::AttInvalid);
    const OledPage p = build(in);
    TEST_ASSERT_EQUAL_STRING("int --- at 0", p.lines[2]);
    TEST_ASSERT_EQUAL_STRING("MPU ok BAR ok MAG --", p.lines[3]);
    TEST_ASSERT_EQUAL_STRING("cal: SIM loop 180us", p.lines[4]);
    TEST_ASSERT_EQUAL_STRING("ALARME ATIT INVALIDA", p.lines[5]);
}

// Valores largos (os extremos dos sensores e os da bancada do rádio): cada linha sai inteira, sem corte
static void test_lines_fit() {
    OledInputs in = base();
    in.pkt.roll = -179.9f;
    in.pkt.pitch = -89.9f;
    in.pkt.altitude_m = -1234.56f;
    in.pkt.pressure_hpa = 1100.0f;
    in.pkt.temp_c = -40.0f;
    in.pkt.gz = -500.0f;
    in.pkt.loop_us = 65535;
    in.pkt.status |= ST_CALIBRATING;
    in.link.recPerS = 100;
    in.link.lostTotal = 1049;
    in.link.lossPct = 16.999f;
    in.link.timeouts = 11;
    in.link.intMinMs = 1.1f;
    in.link.intAvgMs = 165.2f;
    in.link.intMaxMs = 450.6f;
    in.alarmText = alarmKindShort(AlarmKind::AttInvalid);
    in.delayMs = 123456;   // mostrado como 9999
    in.page = 0;
    const OledPage p0 = build(in);
    TEST_ASSERT_EQUAL_STRING("ROLL-179.9 PIT -89.9", p0.lines[1]);
    TEST_ASSERT_EQUAL_STRING("ALT  -1234.56 m", p0.lines[3]);
    TEST_ASSERT_EQUAL_STRING("1100.0hPa  -40.0C", p0.lines[4]);
    TEST_ASSERT_EQUAL_STRING("CURVA -500.0 g/s", p0.lines[5]);
    in.page = 1;
    const OledPage p1 = build(in);
    TEST_ASSERT_EQUAL_STRING("LINK OK 100/s     2/2", p1.lines[0]);
    TEST_ASSERT_EQUAL_STRING("perd 1049 17.0% q11", p1.lines[1]);
    TEST_ASSERT_EQUAL_STRING("int 1/165/451 at 9999", p1.lines[2]);
    TEST_ASSERT_EQUAL_STRING("cal: SIM loop 65535us", p1.lines[4]);
    TEST_ASSERT_EQUAL_STRING("ALARME ATIT INVALIDA", p1.lines[5]);
}

static void test_alarm_short_texts_fit() {
    const AlarmKind kinds[] = {AlarmKind::None, AlarmKind::BankCaution, AlarmKind::AttitudeWarning,
                               AlarmKind::AttInvalid, AlarmKind::DataLate, AlarmKind::LinkLost};
    for (AlarmKind k : kinds) TEST_ASSERT_TRUE(strlen("ALARME ") + strlen(alarmKindShort(k)) <= (size_t)OLED_COLS);
}

// Rodapé da pressão longa: o que vai acontecer, o andamento e o resultado por showMs
static void test_encoder_footer() {
    char b[OLED_COLS + 1];
    TEST_ASSERT_EQUAL_STRING("segure 1s: ZERO ATT",
                             oledEncoderFooter(b, sizeof(b), true, CMD_ZERO_ATTITUDE, false, 0, CmdState::Free, false, 0, 4000));
    TEST_ASSERT_EQUAL_STRING("segure 1s: ZERO ALT",
                             oledEncoderFooter(b, sizeof(b), true, CMD_ZERO_ALTITUDE, false, 0, CmdState::Free, false, 0, 4000));
    TEST_ASSERT_NULL(oledEncoderFooter(b, sizeof(b), false, CMD_ZERO_ATTITUDE, false, 0, CmdState::Free, false, 0, 4000));
    // Em curso: mostrado mesmo depois de showMs
    TEST_ASSERT_EQUAL_STRING("ZERO ALT: ACEITO", oledEncoderFooter(b, sizeof(b), false, 0, true, CMD_ZERO_ALTITUDE,
                                                                   CmdState::Accepted, false, 9000, 4000));
    TEST_ASSERT_EQUAL_STRING("ZERO ATT: OK", oledEncoderFooter(b, sizeof(b), false, 0, true, CMD_ZERO_ATTITUDE,
                                                               CmdState::Done, false, 3999, 4000));
    TEST_ASSERT_NULL(oledEncoderFooter(b, sizeof(b), false, 0, true, CMD_ZERO_ATTITUDE, CmdState::Done, false, 4000, 4000));
    TEST_ASSERT_EQUAL_STRING("ZERO ATT: SEM RESP.", oledEncoderFooter(b, sizeof(b), false, 0, true, CMD_ZERO_ATTITUDE,
                                                                      CmdState::NoResponse, false, 10, 4000));
    TEST_ASSERT_EQUAL_STRING("ZERO ATT: OCUPADO", oledEncoderFooter(b, sizeof(b), false, 0, true, CMD_ZERO_ATTITUDE,
                                                                    CmdState::Rejected, true, 10, 4000));
    // O rodapé substitui a última linha da página
    OledInputs in = base();
    in.footer = "ZERO ATT: OK";
    TEST_ASSERT_EQUAL_STRING("ZERO ATT: OK", build(in).lines[5]);
    in.page = 1;
    TEST_ASSERT_EQUAL_STRING("ZERO ATT: OK", build(in).lines[5]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_encoder_footer);
    RUN_TEST(test_data_page);
    RUN_TEST(test_link_page);
    RUN_TEST(test_no_signal);
    RUN_TEST(test_link_lost);
    RUN_TEST(test_delay_never_cut);
    RUN_TEST(test_data_late);
    RUN_TEST(test_invalid_flags);
    RUN_TEST(test_pressure_needs_val_alt);
    RUN_TEST(test_attitude_degraded);
    RUN_TEST(test_non_finite_attitude);
    RUN_TEST(test_heading_gyro_only_and_wrap);
    RUN_TEST(test_heading_true);
    RUN_TEST(test_signs_and_rounding);
    RUN_TEST(test_link_page_states);
    RUN_TEST(test_lines_fit);
    RUN_TEST(test_alarm_short_texts_fit);
    return UNITY_END();
}
