// status_view.cpp — ver status_view.h
#include "status_view.h"
#include <stdio.h>
#include <string.h>

static const uint32_t RATE_WINDOW_MS = 1000;
static const uint32_t RATE_MAX = 99;                 // 2 dígitos (o rádio vai a 50/s): a L2 cabe nos 128 px
static const uint32_t SPURIOUS_TEXT_MAX = 9999;      // 4 dígitos: o aviso cabe nos 128 px

size_t utf8Columns(const char *s) {
    size_t n = 0;
    for (; *s != '\0'; s++)
        if (((uint8_t)*s & 0xC0) != 0x80) n++;   // bytes de continuação (10xxxxxx) não contam
    return n;
}

void formatUptime(uint32_t s, char *out, size_t n) {
    snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
             (unsigned long)(s % 60));
}

// Monta uma linha em partes, marcando as colunas em vídeo inverso
namespace {
struct LineBuilder {
    char     *buf;
    uint32_t &inv;
    size_t   len = 0;
    size_t   col = 0;

    LineBuilder(char *b, uint32_t &i) : buf(b), inv(i) {
        buf[0] = '\0';
        inv = 0;
    }
    void add(const char *s, bool inverse = false) {
        const size_t cols = utf8Columns(s);
        const size_t bytes = strlen(s);
        if (len + bytes >= STATUS_LINE_BYTES) return;   // não acontece com os textos daqui (testes)
        memcpy(buf + len, s, bytes + 1);
        len += bytes;
        if (inverse)
            for (size_t c = col; c < col + cols && c < 32; c++) inv |= 1UL << c;
        col += cols;
    }
};
}   // namespace

void StatusView::update(const StatusInputs &in, uint32_t nowMs) {
    in_ = in;
    // Taxas do rádio: diferença dos contadores a cada segundo (a subtração sem sinal atravessa a virada)
    if (in.radio != RadioState::Active) {
        haveRef_ = haveRates_ = false;
    } else if (!haveRef_) {
        haveRef_ = true;
        refMs_ = nowMs;
        refSent_ = in.sent;
        refDelivered_ = in.delivered;
    } else {
        const uint32_t el = nowMs - refMs_;
        if (el >= RATE_WINDOW_MS) {
            sentPerS_ = (uint32_t)(((uint64_t)(in.sent - refSent_) * 1000 + el / 2) / el);
            deliveredPerS_ = (uint32_t)(((uint64_t)(in.delivered - refDelivered_) * 1000 + el / 2) / el);
            haveRates_ = true;
            refMs_ = nowMs;
            refSent_ = in.sent;
            refDelivered_ = in.delivered;
        }
    }
    // Pulsos falsos: o aviso fica por spuriousShowMs depois do último aumento do total
    if (in.intSpurious != lastSpurious_) {
        lastSpurious_ = in.intSpurious;
        spuriousMs_ = nowMs;
        spuriousSeen_ = true;
    }
    spuriousRecent_ = spuriousSeen_ && nowMs - spuriousMs_ < p_.spuriousShowMs;
}

int StatusView::warnings(const char *out[3], char *spuriousText, size_t n) const {
    int k = 0;
    if (in_.haveState && in_.calibrating) out[k++] = "CALIBRANDO";
    switch (in_.lastReset) {
    case ResetKind::Brownout: out[k++] = "REINÍCIO: BROWNOUT"; break;
    case ResetKind::Panic:    out[k++] = "REINÍCIO: PÂNICO"; break;
    case ResetKind::Watchdog: out[k++] = "REINÍCIO: WATCHDOG"; break;
    case ResetKind::Normal:   break;
    }
    if (spuriousRecent_) {
        if (lastSpurious_ <= SPURIOUS_TEXT_MAX)
            snprintf(spuriousText, n, "INT: %lu PULSOS FALSOS", (unsigned long)lastSpurious_);
        else
            snprintf(spuriousText, n, "INT: >%lu PULSOS FALSOS", (unsigned long)SPURIOUS_TEXT_MAX);
        out[k++] = spuriousText;
    }
    return k;
}

void StatusView::render(uint32_t nowMs, StatusScreen &out) const {
    char tmp[STATUS_LINE_BYTES];

    // L1: sensores
    LineBuilder l1(out.line[0], out.inverse[0]);
    if (!in_.haveState) {
        l1.add("aguardando sensores");
    } else {
        l1.add("MPU ");
        l1.add(in_.mpuOk ? "OK" : "--", !in_.mpuOk);
        l1.add("  BARO ");
        l1.add(in_.baroOk ? "OK" : "--", !in_.baroOk);
        l1.add("  MAG ");
        l1.add(in_.magOk ? "OK" : "--", !in_.magOk);
    }

    // L2: link
    LineBuilder l2(out.line[1], out.inverse[1]);
    switch (in_.radio) {
    case RadioState::Off:      l2.add("RÁDIO DESLIGADO"); break;
    case RadioState::Starting: l2.add("RÁDIO LIGANDO..."); break;
    case RadioState::Active:
        if (haveRates_) {
            snprintf(tmp, sizeof(tmp), "TX %lu/s  ENTREGUES %lu/s",
                     (unsigned long)(sentPerS_ > RATE_MAX ? RATE_MAX : sentPerS_),
                     (unsigned long)(deliveredPerS_ > RATE_MAX ? RATE_MAX : deliveredPerS_));
            l2.add(tmp);
        } else {
            l2.add("TX --/s  ENTREGUES --/s");
        }
        break;
    }

    // L3: tempo ligado
    LineBuilder l3(out.line[2], out.inverse[2]);
    l3.add("LIGADO ");
    formatUptime(in_.uptimeS, tmp, sizeof(tmp));
    l3.add(tmp);

    // L4: avisos, um por vez
    LineBuilder l4(out.line[3], out.inverse[3]);
    const char *w[3];
    char spuriousText[STATUS_LINE_BYTES];
    const int k = warnings(w, spuriousText, sizeof(spuriousText));
    if (k == 0) l4.add("SEM AVISOS");
    else l4.add(w[(nowMs / p_.warnRotateMs) % (uint32_t)k], true);
}

LedMode StatusView::ledMode() const {
    if (in_.radio != RadioState::Active) return LedMode::Off;
    return haveRates_ && deliveredPerS_ > 0 ? LedMode::Delivering : LedMode::NoDelivery;
}

bool StatusView::ledOn(uint32_t nowMs) const {
    switch (ledMode()) {
    case LedMode::Delivering: return nowMs % p_.ledPeriodMs < p_.ledOnMs;
    case LedMode::NoDelivery: return nowMs % p_.ledIdlePeriodMs < p_.ledIdleOnMs;
    case LedMode::Off:        break;
    }
    return false;
}
