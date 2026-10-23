// Testes das condições de alarme e dos padrões dos LEDs (lib/alarm_logic).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <math.h>
#include "alarm_logic.h"

void setUp() {}
void tearDown() {}

static const AlarmConfig CFG = {35.0f, 45.0f, 25.0f, 10.0f, 3.0f, 1000, 500, 250, 100, 2000, 100};

// Link OK, atitude válida e nivelada
static AlarmInputs level() {
    AlarmInputs in;
    in.everLinked = true;
    in.linkOk = true;
    in.attValid = true;
    in.calibrating = false;
    in.rollDeg = 0.0f;
    in.pitchDeg = 0.0f;
    return in;
}

static AlarmInputs att(float roll, float pitch) {
    AlarmInputs in = level();
    in.rollDeg = roll;
    in.pitchDeg = pitch;
    return in;
}

static void test_normal() {
    AlarmLogic a(CFG);
    const AlarmOutput o = a.update(level(), 1000);
    TEST_ASSERT_EQUAL(AlarmKind::None, o.kind);
    TEST_ASSERT_FALSE(o.alarmLed);
    TEST_ASSERT_TRUE(o.linkLed);
}

// Antes do primeiro pacote: sem alarme e LED do link apagado
static void test_no_signal_no_alarm() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.everLinked = false;
    in.linkOk = false;
    const AlarmOutput o = a.update(in, 5000);
    TEST_ASSERT_EQUAL(AlarmKind::None, o.kind);
    TEST_ASSERT_FALSE(o.linkLed);
    TEST_ASSERT_FALSE(o.alarmLed);
}

static void test_link_lost() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.linkOk = false;
    const AlarmOutput o = a.update(in, 1000);
    TEST_ASSERT_EQUAL(AlarmKind::LinkLost, o.kind);
    TEST_ASSERT_FALSE(o.linkLed);
    // 1 Hz: aceso de 1000 a 1499, apagado de 1500 a 1999
    TEST_ASSERT_TRUE(a.update(in, 1499).alarmLed);
    TEST_ASSERT_FALSE(a.update(in, 1500).alarmLed);
    TEST_ASSERT_FALSE(a.update(in, 1999).alarmLed);
    TEST_ASSERT_TRUE(a.update(in, 2000).alarmLed);
    // Voltou: sem alarme
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(level(), 2100).kind);
}

// Link perdido tem prioridade, mesmo com o último pacote inclinado
static void test_link_lost_has_priority() {
    AlarmLogic a(CFG);
    AlarmInputs in = att(60.0f, 0.0f);
    in.linkOk = false;
    TEST_ASSERT_EQUAL(AlarmKind::LinkLost, a.update(in, 1000).kind);
}

static void test_bank_caution_and_hysteresis() {
    AlarmLogic a(CFG);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(35.0f, 0), 0).kind);   // no limite: não entra
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(35.1f, 0), 20).kind);
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(-33.0f, 0), 40).kind);   // simétrico; acima de 32
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(32.0f, 0), 60).kind);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(31.9f, 0), 80).kind);
    // Esquerda também
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(-36.0f, 0), 100).kind);
    // 2 Hz, com a fase contada desde a entrada (100 ms)
    TEST_ASSERT_TRUE(a.update(att(-36.0f, 0), 1100).alarmLed);
    TEST_ASSERT_FALSE(a.update(att(-36.0f, 0), 1350).alarmLed);
}

static void test_bank_warning_and_hysteresis() {
    AlarmLogic a(CFG);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(45.5f, 0), 0).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(42.0f, 0), 20).kind);   // ainda não abaixo de 42
    // Abaixo de 42: sai do alarme e cai para o aviso (ainda acima de 35)
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(41.9f, 0), 40).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(-46.0f, 0), 60).kind);
    // 5 Hz, com a fase contada desde a entrada (60 ms)
    TEST_ASSERT_TRUE(a.update(att(-46.0f, 0), 1060).alarmLed);
    TEST_ASSERT_FALSE(a.update(att(-46.0f, 0), 1160).alarmLed);
}

static void test_pitch_limits_asymmetric() {
    AlarmLogic a(CFG);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(0, 24.9f), 0).kind);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(0, -9.9f), 20).kind);   // −10 para baixo, e não −25
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(0, -10.1f), 40).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(0, -7.0f), 60).kind);   // sai acima de −7
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(0, -6.9f), 80).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(0, 25.1f), 100).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(0, 22.0f), 120).kind);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(att(0, 21.9f), 140).kind);
}

// O alarme de atitude anormal tem prioridade sobre o aviso de bank
static void test_warning_over_caution() {
    AlarmLogic a(CFG);
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, a.update(att(38.0f, 26.0f), 0).kind);
}

static void test_att_invalid_delay() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.attValid = false;
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 1000).kind);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 1999).kind);
    const AlarmOutput o = a.update(in, 2000);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, o.kind);
    TEST_ASSERT_TRUE(o.linkLed);
    // Duas piscadas: 0–99 aceso, 100–199 apagado, 200–299 aceso, depois apagado até 2000
    TEST_ASSERT_TRUE(a.update(in, 4000).alarmLed);
    TEST_ASSERT_FALSE(a.update(in, 4150).alarmLed);
    TEST_ASSERT_TRUE(a.update(in, 4250).alarmLed);
    TEST_ASSERT_FALSE(a.update(in, 4300).alarmLed);
    TEST_ASSERT_FALSE(a.update(in, 5900).alarmLed);
    // Volta válida: sai
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(level(), 6000).kind);
}

// Uma falta curta da atitude não alarma, e o tempo recomeça a cada falta
static void test_att_invalid_short_gaps() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.attValid = false;
    a.update(in, 0);
    a.update(level(), 900);
    a.update(in, 1000);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 1900).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, a.update(in, 2000).kind);
}

// Durante uma calibração do A, a atitude inválida é esperada: sem alarme
static void test_att_invalid_ignored_while_calibrating() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.attValid = false;
    in.calibrating = true;
    a.update(in, 0);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 5000).kind);
    // Acabou a calibração e a atitude continua inválida: o tempo começa agora
    in.calibrating = false;
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 5100).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, a.update(in, 6100).kind);
}

// Com a atitude inválida, os limites não são avaliados (o último ângulo não vale)
static void test_limits_need_valid_attitude() {
    AlarmLogic a(CFG);
    AlarmInputs in = att(60.0f, 0);
    in.attValid = false;
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 0).kind);
}

static void test_acknowledge_and_rearm() {
    AlarmLogic a(CFG);
    TEST_ASSERT_FALSE(a.acknowledge());   // nada a reconhecer
    a.update(att(50.0f, 0), 0);
    TEST_ASSERT_TRUE(a.acknowledge());
    // Reconhecido: aceso fixo, inclusive na fase apagada do padrão
    const AlarmOutput o = a.update(att(50.0f, 0), 100);
    TEST_ASSERT_TRUE(o.acked);
    TEST_ASSERT_TRUE(o.alarmLed);
    // Caiu para o aviso: condição diferente, volta a piscar
    AlarmOutput o2 = a.update(att(40.0f, 0), 120);
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, o2.kind);
    TEST_ASSERT_FALSE(o2.acked);
    // Terminou e voltou: alarma de novo
    a.acknowledge();
    a.update(att(0, 0), 140);
    o2 = a.update(att(36.0f, 0), 160);
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, o2.kind);
    TEST_ASSERT_FALSE(o2.acked);
}

// Sem atitude utilizável, o estado da histerese recomeça: ao voltar perto do limite, não fica preso no alarme
static void test_hysteresis_reset_on_invalid() {
    AlarmLogic a(CFG);
    a.update(att(46.0f, 0), 0);
    AlarmInputs lost = att(46.0f, 0);
    lost.linkOk = false;
    a.update(lost, 20);
    TEST_ASSERT_EQUAL(AlarmKind::BankCaution, a.update(att(43.0f, 0), 40).kind);
}

// Um alarme novo acende na hora, qualquer que seja o instante (a fase conta da mudança de condição)
static void test_new_alarm_starts_on() {
    AlarmLogic a(CFG);
    AlarmInputs in = level();
    in.attValid = false;
    a.update(level(), 0);
    a.update(in, 1300);   // começa a contar a atitude inválida
    const AlarmOutput o = a.update(in, 2300);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, o.kind);
    TEST_ASSERT_TRUE(o.alarmLed);   // pela fase absoluta, ficaria apagado até 4000
    // Os outros padrões também começam acesos
    AlarmLogic b(CFG);
    b.update(level(), 0);
    TEST_ASSERT_TRUE(b.update(att(36.0f, 0), 1260).alarmLed);   // 1260 cairia na fase apagada a 2 Hz
    AlarmLogic c(CFG);
    AlarmInputs lost = level();
    c.update(level(), 0);
    lost.linkOk = false;
    TEST_ASSERT_TRUE(c.update(lost, 1700).alarmLed);   // 1700 cairia na fase apagada a 1 Hz
}

// Ângulo não finito com VAL_ATT ligada: não avalia os limites e conta como atitude inválida
static void test_non_finite_angles_are_invalid() {
    AlarmLogic a(CFG);
    AlarmInputs in = att(NAN, 0.0f);
    TEST_ASSERT_EQUAL(AlarmKind::None, a.update(in, 0).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, a.update(in, 1000).kind);
    AlarmLogic b(CFG);
    AlarmInputs in2 = att(50.0f, INFINITY);   // roll acima do limite, mas o pacote é incoerente
    TEST_ASSERT_EQUAL(AlarmKind::None, b.update(in2, 0).kind);
    TEST_ASSERT_EQUAL(AlarmKind::AttInvalid, b.update(in2, 1000).kind);
    // Voltou finito: avalia normalmente
    TEST_ASSERT_EQUAL(AlarmKind::AttitudeWarning, b.update(att(50.0f, 0), 1020).kind);
}

static void test_kind_text() {
    TEST_ASSERT_EQUAL_STRING("nenhum", alarmKindText(AlarmKind::None));
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO", alarmKindText(AlarmKind::LinkLost));
    TEST_ASSERT_EQUAL_STRING("ATITUDE ANORMAL", alarmKindText(AlarmKind::AttitudeWarning));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal);
    RUN_TEST(test_no_signal_no_alarm);
    RUN_TEST(test_link_lost);
    RUN_TEST(test_link_lost_has_priority);
    RUN_TEST(test_bank_caution_and_hysteresis);
    RUN_TEST(test_bank_warning_and_hysteresis);
    RUN_TEST(test_pitch_limits_asymmetric);
    RUN_TEST(test_warning_over_caution);
    RUN_TEST(test_att_invalid_delay);
    RUN_TEST(test_att_invalid_short_gaps);
    RUN_TEST(test_att_invalid_ignored_while_calibrating);
    RUN_TEST(test_limits_need_valid_attitude);
    RUN_TEST(test_acknowledge_and_rearm);
    RUN_TEST(test_hysteresis_reset_on_invalid);
    RUN_TEST(test_new_alarm_starts_on);
    RUN_TEST(test_non_finite_angles_are_invalid);
    RUN_TEST(test_kind_text);
    return UNITY_END();
}
