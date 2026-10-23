// Testes da escolha do estado visual do TFT (lib/pfd_state).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <math.h>
#include "pfd_state.h"
#include "telemetry.h"

void setUp() {}
void tearDown() {}

static PfdInputs ok() {
    PfdInputs in;
    in.everLinked = true;
    in.linkOk = true;
    in.dataLate = false;
    in.valid = VAL_ATT | VAL_ALT | VAL_HDG;
    in.roll = 2.0f;
    in.pitch = -1.0f;
    in.heading = 95.0f;
    return in;
}

static void test_normal() {
    const PfdState s = pfdState(ok());
    TEST_ASSERT_EQUAL(PfdFlag::None, s.flag);
    TEST_ASSERT_FALSE(s.degraded);
    TEST_ASSERT_TRUE(s.headingOk);
    TEST_ASSERT_FALSE(s.gyroOnly);
    TEST_ASSERT_EQUAL_STRING("", pfdFlagText(s.flag));
}

static void test_no_signal() {
    PfdInputs in = ok();
    in.everLinked = false;
    in.linkOk = false;
    const PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::NoSignal, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    TEST_ASSERT_EQUAL_STRING("SEM SINAL", pfdFlagText(s.flag));
}

static void test_link_lost() {
    PfdInputs in = ok();
    in.linkOk = false;
    in.dataLate = true;   // perdido tem prioridade sobre atrasado
    const PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::LinkLost, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    TEST_ASSERT_EQUAL_STRING("LINK PERDIDO", pfdFlagText(s.flag));
}

static void test_data_late() {
    PfdInputs in = ok();
    in.dataLate = true;
    const PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::DataLate, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    TEST_ASSERT_EQUAL_STRING("DADO ATRASADO", pfdFlagText(s.flag));
}

static void test_att_invalid_flag() {
    PfdInputs in = ok();
    in.valid = VAL_ALT;   // sem VAL_ATT (e, no A, sem VAL_HDG)
    const PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::AttInvalid, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    TEST_ASSERT_EQUAL_STRING("ATT INVALIDA", pfdFlagText(s.flag));
}

// Caso da revisão: roll NaN com VAL_ATT e VAL_HDG ligadas e proa válida. Bandeira, e a proa vai para "---"
// (nunca um número junto do X vermelho)
static void test_incoherent_packet_hides_heading() {
    PfdInputs in = ok();
    in.roll = NAN;
    PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::AttInvalid, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    in = ok();
    in.pitch = INFINITY;
    in.valid |= VAL_HDG_GYRO_ONLY;
    s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::AttInvalid, s.flag);
    TEST_ASSERT_FALSE(s.headingOk);
    TEST_ASSERT_FALSE(s.gyroOnly);
}

static void test_degraded() {
    PfdInputs in = ok();
    in.valid |= VAL_ATT_DEGRADED;
    PfdState s = pfdState(in);
    TEST_ASSERT_EQUAL(PfdFlag::None, s.flag);
    TEST_ASSERT_TRUE(s.degraded);
    // Com bandeira, a marca não aparece (a atitude não é mostrada)
    in.dataLate = true;
    s = pfdState(in);
    TEST_ASSERT_FALSE(s.degraded);
}

static void test_heading_rules() {
    PfdInputs in = ok();
    in.valid |= VAL_HDG_GYRO_ONLY;
    PfdState s = pfdState(in);
    TEST_ASSERT_TRUE(s.headingOk);
    TEST_ASSERT_TRUE(s.gyroOnly);
    in = ok();
    in.valid &= ~VAL_HDG;
    TEST_ASSERT_FALSE(pfdState(in).headingOk);
    in = ok();
    in.heading = NAN;
    TEST_ASSERT_FALSE(pfdState(in).headingOk);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal);
    RUN_TEST(test_no_signal);
    RUN_TEST(test_link_lost);
    RUN_TEST(test_data_late);
    RUN_TEST(test_att_invalid_flag);
    RUN_TEST(test_incoherent_packet_hides_heading);
    RUN_TEST(test_degraded);
    RUN_TEST(test_heading_rules);
    return UNITY_END();
}
