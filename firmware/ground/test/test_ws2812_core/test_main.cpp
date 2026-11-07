// Testes da sequência de envio e recuperação do driver da fita (lib/ws2812_core), com o RMT simulado: prazo
// vencido, erro no envio, reinstalação e desinstalação que falham. Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "ws2812_core.h"

// RMT simulado: cada operação registra uma letra no log e pode falhar quando o teste pede
struct FakeRmt {
    char      log[64];
    int       n;
    bool      installOk, writeOk, waitOk, stopOk, uninstallOk;
    int       writes;
    uint8_t   lastWrite[12];
    Ws2812Driver *drv;
    bool      readyDuringStop;   // o driver já estava "não pronto" quando a recuperação parou o hardware?
};

static void mark(FakeRmt *f, char c) {
    if (f->n < 63) f->log[f->n++] = c;
    f->log[f->n] = '\0';
}
static bool fInstall(void *c) { auto *f = (FakeRmt *)c; mark(f, 'I'); return f->installOk; }
static bool fWrite(void *c, const uint8_t *b, size_t n) {
    auto *f = (FakeRmt *)c;
    mark(f, 'W');
    f->writes++;
    memcpy(f->lastWrite, b, n < sizeof(f->lastWrite) ? n : sizeof(f->lastWrite));
    return f->writeOk;
}
static bool fWait(void *c, uint32_t) { auto *f = (FakeRmt *)c; mark(f, 'T'); return f->waitOk; }
static bool fStop(void *c) {
    auto *f = (FakeRmt *)c;
    mark(f, 'S');
    f->readyDuringStop = f->drv->ready();
    return f->stopOk;
}
static bool fUninstall(void *c) { auto *f = (FakeRmt *)c; mark(f, 'U'); return f->uninstallOk; }

static FakeRmt s_f;
static Ws2812Driver *s_d;
static uint8_t s_buf[12];
static const LedRgb FRAME[4] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};

static void clearLog() {
    s_f.n = 0;
    s_f.log[0] = '\0';
}

void setUp() {
    static Ws2812Driver d;
    d = Ws2812Driver();
    s_d = &d;
    memset(&s_f, 0, sizeof(s_f));
    s_f.installOk = s_f.writeOk = s_f.waitOk = s_f.stopOk = s_f.uninstallOk = true;
    s_f.drv = s_d;
    memset(s_buf, 0xAA, sizeof(s_buf));
    const Ws2812Ops ops = {&s_f, fInstall, fWrite, fWait, fStop, fUninstall};
    TEST_ASSERT_TRUE(s_d->begin(ops, s_buf, 4, true));
    clearLog();
}
void tearDown() {}

static void test_send_ok_grb() {
    TEST_ASSERT_TRUE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_STRING("WT", s_f.log);
    TEST_ASSERT_EQUAL_UINT8(20, s_f.lastWrite[0]);   // G, R, B
    TEST_ASSERT_EQUAL_UINT8(10, s_f.lastWrite[1]);
    TEST_ASSERT_EQUAL_UINT8(30, s_f.lastWrite[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s_d->failures());
}

static void test_timeout_recovers_and_next_frame_goes_out() {
    s_f.waitOk = false;
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 0));
    TEST_ASSERT_EQUAL_STRING("WTSUI", s_f.log);      // envio, espera vencida, para, desinstala, instala
    TEST_ASSERT_FALSE(s_f.readyDuringStop);           // "não pronto" antes de começar a recuperação
    TEST_ASSERT_EQUAL_UINT32(1, s_d->failures());
    TEST_ASSERT_EQUAL_UINT32(1, s_d->recoveries());
    TEST_ASSERT_EQUAL_UINT32(0, s_d->recoveryFailures());
    TEST_ASSERT_TRUE(s_d->ready());
    s_f.waitOk = true;
    clearLog();
    TEST_ASSERT_TRUE(s_d->show(FRAME, 4, 20));        // o próximo quadro sai normalmente
    TEST_ASSERT_EQUAL_STRING("WT", s_f.log);
}

static void test_write_error_recovers() {
    s_f.writeOk = false;
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_STRING("WSUI", s_f.log);       // sem espera: o envio nem começou
    TEST_ASSERT_TRUE(s_d->ready());
}

static void test_stop_failure_still_uninstalls() {
    s_f.waitOk = false;
    s_f.stopOk = false;
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_STRING("WTSUI", s_f.log);
    TEST_ASSERT_TRUE(s_d->ready());
}

static void test_failed_reinstall_returns_immediately_without_touching_buffer() {
    s_f.waitOk = false;
    s_f.installOk = false;
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL(Ws2812State::NotInstalled, s_d->state());
    TEST_ASSERT_EQUAL_UINT32(1, s_d->recoveryFailures());
    // Envios seguintes: voltam na hora, sem nenhuma operação do RMT e sem reescrever o buffer
    uint8_t before[12];
    memcpy(before, s_buf, sizeof(before));
    clearLog();
    const LedRgb other[4] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {1, 1, 1}};
    TEST_ASSERT_FALSE(s_d->show(other, 4, 20));
    TEST_ASSERT_FALSE(s_d->show(other, 4, 20));
    TEST_ASSERT_EQUAL_STRING("", s_f.log);
    TEST_ASSERT_EQUAL_MEMORY(before, s_buf, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(1, s_d->failures());     // só a falha original conta
    // Nova tentativa que falha de novo, depois uma que funciona
    TEST_ASSERT_FALSE(s_d->retry());
    TEST_ASSERT_EQUAL(Ws2812State::NotInstalled, s_d->state());
    s_f.installOk = true;
    s_f.waitOk = true;
    TEST_ASSERT_TRUE(s_d->retry());
    TEST_ASSERT_TRUE(s_d->ready());
    clearLog();
    TEST_ASSERT_TRUE(s_d->show(other, 4, 20));
    TEST_ASSERT_EQUAL_STRING("WT", s_f.log);
}

static void test_failed_uninstall_locks_buffer_forever() {
    s_f.waitOk = false;
    s_f.uninstallOk = false;
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_STRING("WTSU", s_f.log);       // não tenta instalar por cima de um driver vivo
    TEST_ASSERT_EQUAL(Ws2812State::BufferLocked, s_d->state());
    uint8_t before[12];
    memcpy(before, s_buf, sizeof(before));
    clearLog();
    TEST_ASSERT_FALSE(s_d->show(FRAME, 4, 20));
    TEST_ASSERT_FALSE(s_d->retry());                  // nem uma nova tentativa: a interrupção pode ler o buffer
    TEST_ASSERT_EQUAL_STRING("", s_f.log);
    TEST_ASSERT_EQUAL_MEMORY(before, s_buf, sizeof(before));
}

static void test_begin_failure_and_rgb_order() {
    Ws2812Driver d;
    FakeRmt f;
    memset(&f, 0, sizeof(f));
    f.drv = &d;
    f.writeOk = f.waitOk = true;
    f.installOk = false;
    uint8_t buf[12];
    const Ws2812Ops ops = {&f, fInstall, fWrite, fWait, fStop, fUninstall};
    TEST_ASSERT_FALSE(d.begin(ops, buf, 4, false));
    TEST_ASSERT_EQUAL(Ws2812State::Off, d.state());
    TEST_ASSERT_FALSE(d.show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_INT(0, f.writes);
    TEST_ASSERT_FALSE(d.retry());                     // retry só depois de uma recuperação
    // Ordem RGB
    Ws2812Driver d2;
    f.installOk = true;
    f.drv = &d2;
    TEST_ASSERT_TRUE(d2.begin(ops, buf, 4, false));
    TEST_ASSERT_TRUE(d2.show(FRAME, 4, 20));
    TEST_ASSERT_EQUAL_UINT8(10, f.lastWrite[0]);
    TEST_ASSERT_EQUAL_UINT8(20, f.lastWrite[1]);
}

static void test_count_is_clamped() {
    LedRgb big[8] = {};
    TEST_ASSERT_TRUE(s_d->show(big, 8, 20));          // não escreve além do buffer de 4 LEDs
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_ok_grb);
    RUN_TEST(test_timeout_recovers_and_next_frame_goes_out);
    RUN_TEST(test_write_error_recovers);
    RUN_TEST(test_stop_failure_still_uninstalls);
    RUN_TEST(test_failed_reinstall_returns_immediately_without_touching_buffer);
    RUN_TEST(test_failed_uninstall_locks_buffer_forever);
    RUN_TEST(test_begin_failure_and_rgb_order);
    RUN_TEST(test_count_is_clamped);
    return UNITY_END();
}
