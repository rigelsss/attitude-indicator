// Testes do núcleo do logger (lib/log_line, LogCore). Rodar no PC: pio test -e native
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <functional>
#include <string>
#include "log_core.h"

void setUp() {}
void tearDown() {}

// Buffer linear de capacidade fixa: mensagens inteiras ou nada, como o esp_ringbuf de bytes
class FakeBuffer : public LogBuffer {
public:
    explicit FakeBuffer(size_t cap) : cap_(cap) {}
    bool send(const char *s, size_t len) override {
        if (data_.size() + len > cap_) return false;
        data_.append(s, len);
        return true;
    }
    size_t freeBytes() const override { return cap_ - data_.size(); }
    const char *acquire(size_t max, size_t &n) override {
        if (data_.empty()) return nullptr;
        n = data_.size() < max ? data_.size() : max;
        held_ = n;
        return data_.data();
    }
    void release(const char *) override { data_.erase(0, held_); }

private:
    size_t cap_;
    std::string data_;
    size_t held_ = 0;
};

// Saída com espaço controlado pelo teste
class FakeSink : public LogSink {
public:
    size_t roomLeft = 1000;
    std::string text;
    size_t room() override { return roomLeft; }
    void write(const char *s, size_t len) override {
        text.append(s, len);
        roomLeft -= len;
    }
};

// Conta a profundidade e permite simular uma preempção: preempt roda uma vez, logo depois do
// unlock() de número preemptAt (contado a partir de 1), com a trava livre
class FakeLock : public LogLock {
public:
    int depth = 0;
    int maxDepth = 0;
    int unlocks = 0;
    int preemptAt = 0;
    std::function<void()> preempt;
    void lock() override {
        depth++;
        if (depth > maxDepth) maxDepth = depth;
    }
    void unlock() override {
        depth--;
        unlocks++;
        if (preempt && unlocks == preemptAt) {
            std::function<void()> f = preempt;
            preempt = nullptr;
            f();
        }
    }
};

static void push(LogCore &core, const std::string &s) { core.push(s.data(), s.size()); }

static std::string note(unsigned msgs, unsigned bytes) {
    char b[96];
    snprintf(b, sizeof(b), "[log] %u mensagens descartadas (%u bytes): buffer cheio\n", msgs, bytes);
    return b;
}

static const std::string A = "0123456789abcdef\n";   // 17 bytes: num buffer de 20, sobram 3
static const std::string X = "descarte\n\n";          // 10 bytes: não cabe depois de A

// Sem descarte: a saída é a entrada, sem aviso
static void test_no_drop_passes_through() {
    FakeBuffer buf(64);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, "um\n");
    push(core, "dois\n");
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING("um\ndois\n", out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, core.pendingMsgs());
    TEST_ASSERT_EQUAL_INT(0, lock.depth);
}

// Encher o buffer, descartar e esvaziar sem mensagens novas: o aviso sai no fim, uma vez só
static void test_last_drop_announced_by_pump() {
    FakeBuffer buf(20);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, A);
    push(core, X);
    TEST_ASSERT_EQUAL_UINT32(1, core.pendingMsgs());
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(1, 10)).c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, core.pendingMsgs());
    core.pump(out);
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(1, 10)).c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_INT(0, lock.depth);
}

// A intercalação da revisão: descarte, depois uma mensagem B que caberia no espaço livre. B não pode sair
// antes do aviso: com o descarte pendente, ela entra no aviso
static void test_message_after_drop_does_not_overtake_note() {
    FakeBuffer buf(20);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, A);
    push(core, X);       // descartada
    push(core, "B\n");   // 2 bytes cabem nos 3 livres, mas o descarte está pendente
    TEST_ASSERT_EQUAL_UINT32(2, core.pendingMsgs());
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(2, 12)).c_str(), out.text.c_str());
}

// Preempção logo depois da reserva do aviso pelo consumidor: B entra no buffer e sai depois do aviso
static void test_message_during_note_reservation_comes_after_note() {
    FakeBuffer buf(20);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, A);
    push(core, X);
    lock.unlocks = 0;
    lock.preemptAt = 1;   // no pump, o primeiro unlock é o da reserva (take)
    lock.preempt = [&core]() { push(core, "B\n"); };
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(1, 10)).c_str(), out.text.c_str());
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(1, 10) + "B\n").c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, core.pendingMsgs());
}

// Preempção de outro produtor entre a conferência do descarte e o envio: as duas são concorrentes, e
// qualquer ordem vale; o que não pode é perder a contagem nem estourar o contador
static void test_concurrent_producers_keep_count() {
    FakeBuffer buf(20);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, A);
    lock.unlocks = 0;
    lock.preemptAt = 1;   // depois da conferência de X, antes do envio dele
    lock.preempt = [&core]() { push(core, "Y12345\n"); };   // 7 bytes: também não cabe
    push(core, X);
    TEST_ASSERT_EQUAL_UINT32(2, core.pendingMsgs());
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(2, 17)).c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, core.pendingMsgs());
}

// Saída sem espaço para o aviso: nada é reservado, as mensagens novas seguem entrando no aviso, e ele sai
// uma vez só quando houver espaço
static void test_note_waits_for_room() {
    FakeBuffer buf(20);
    FakeLock lock;
    LogCore core(buf, lock);
    FakeSink out;
    push(core, A);
    push(core, X);
    out.roomLeft = A.size();   // A sai inteira, e o espaço acaba antes de ver o buffer vazio
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING(A.c_str(), out.text.c_str());
    out.roomLeft = 10;         // buffer vazio, mas o aviso não cabe
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING(A.c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, core.pendingMsgs());
    push(core, "C\n");         // ainda com o descarte pendente
    TEST_ASSERT_EQUAL_UINT32(2, core.pendingMsgs());
    out.roomLeft = 1000;
    core.pump(out);
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(2, 12)).c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, core.pendingMsgs());
    // Depois do aviso, as mensagens voltam a ser aceitas
    push(core, "D\n");
    core.pump(out);
    TEST_ASSERT_EQUAL_STRING((A + note(2, 12) + "D\n").c_str(), out.text.c_str());
    TEST_ASSERT_EQUAL_INT(0, lock.depth);
    TEST_ASSERT_EQUAL_INT(1, lock.maxDepth);   // a trava nunca é tomada de dentro dela mesma
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_no_drop_passes_through);
    RUN_TEST(test_last_drop_announced_by_pump);
    RUN_TEST(test_message_after_drop_does_not_overtake_note);
    RUN_TEST(test_message_during_note_reservation_comes_after_note);
    RUN_TEST(test_concurrent_producers_keep_count);
    RUN_TEST(test_note_waits_for_room);
    return UNITY_END();
}
