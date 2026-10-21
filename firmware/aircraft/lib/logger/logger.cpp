// logger.cpp — ver logger.h. A regra do buffer e do descarte está no LogCore (lib/log_line, testado no
// native); aqui ficam os adaptadores: esp_ringbuf, Serial e portMUX.
#include "logger.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/ringbuf.h>
#include <esp_log.h>
#include <esp32/rom/ets_sys.h>
#include "log_core.h"
#include "log_line.h"

// Reserva no buffer de transmissão do Serial: o driver da UART guarda cada escrita com um cabeçalho, e uma
// escrita do tamanho exato do espaço livre poderia esperar
static const int SERIAL_ROOM_MARGIN = 64;

class RingLogBuffer : public LogBuffer {
public:
    RingbufHandle_t rb = nullptr;
    bool send(const char *s, size_t len) override {
        if (xPortInIsrContext()) {
            BaseType_t woke = pdFALSE;
            return xRingbufferSendFromISR(rb, s, len, &woke) == pdTRUE;
        }
        return xRingbufferSend(rb, s, len, 0) == pdTRUE;
    }
    size_t freeBytes() const override { return xRingbufferGetCurFreeSize(rb); }
    const char *acquire(size_t max, size_t &n) override {
        return (const char *)xRingbufferReceiveUpTo(rb, &n, 0, max);
    }
    void release(const char *p) override { vRingbufferReturnItem(rb, (void *)p); }
    size_t waiting() const {
        UBaseType_t n = 0;
        vRingbufferGetInfo(rb, nullptr, nullptr, nullptr, nullptr, &n);
        return n;
    }
};

class SerialLogSink : public LogSink {
public:
    size_t room() override {
        const int r = Serial.availableForWrite() - SERIAL_ROOM_MARGIN;
        return r > 0 ? (size_t)r : 0;
    }
    void write(const char *s, size_t len) override { Serial.write((const uint8_t *)s, len); }
};

class MuxLogLock : public LogLock {
public:
    void lock() override { portENTER_CRITICAL_SAFE(&mux_); }
    void unlock() override { portEXIT_CRITICAL_SAFE(&mux_); }

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

static RingLogBuffer s_buf;
static SerialLogSink s_out;
static MuxLogLock s_dropLock;
static LogCore s_core(s_buf, s_dropLock);
static bool s_ready = false;

static void push(const char *s, size_t len) {
    if (len == 0) return;
    if (!s_ready) {
        Serial.write((const uint8_t *)s, len);
        return;
    }
    s_core.push(s, len);
}

// ---------- Logs do núcleo (ets_printf, um caractere por vez) e do ESP-IDF ----------

static portMUX_TYPE s_lineMux = portMUX_INITIALIZER_UNLOCKED;
static char s_lineBuf[160];
static LineAssembler s_line(s_lineBuf, sizeof(s_lineBuf));

static void corePutc(char c) {
    char out[sizeof(s_lineBuf)];
    size_t n = 0;
    portENTER_CRITICAL_SAFE(&s_lineMux);
    if (s_line.put(c)) {
        n = s_line.size();
        memcpy(out, s_line.data(), n);
        s_line.clear();
    }
    portEXIT_CRITICAL_SAFE(&s_lineMux);
    if (n > 0) push(out, n);   // fora da seção crítica
}

static int idfVprintf(const char *fmt, va_list arg) {
    char buf[192];
    int len = vsnprintf(buf, sizeof(buf), fmt, arg);
    if (len < 0) return len;
    push(buf, len < (int)sizeof(buf) ? len : sizeof(buf) - 1);
    return len;
}

bool logBegin(size_t bytes) {
    s_buf.rb = xRingbufferCreate(bytes, RINGBUF_TYPE_BYTEBUF);
    if (s_buf.rb == nullptr) return false;
    s_ready = true;
    // Desliga o canal de depuração do núcleo no Serial: sem ele, log_printf não espera a UART ficar ociosa.
    // Em seguida, o putc do ets_printf passa a ser o deste arquivo
    Serial.setDebugOutput(false);
    ets_install_putc1(corePutc);
    esp_log_set_vprintf(idfVprintf);
    return true;
}

// ---------- Saída do firmware ----------

// Mesmo esquema do Print::printf do núcleo: buffer de 64 bytes na pilha e, para linhas
// maiores, um buffer alocado do tamanho exato
void logPrintf(const char *fmt, ...) {
    char loc[64];
    char *buf = loc;
    va_list arg, copy;
    va_start(arg, fmt);
    va_copy(copy, arg);
    int len = vsnprintf(loc, sizeof(loc), fmt, copy);
    va_end(copy);
    if (len < 0) {
        va_end(arg);
        return;
    }
    if (len >= (int)sizeof(loc)) {
        buf = (char *)malloc(len + 1);
        if (buf == nullptr) {
            va_end(arg);
            return;
        }
        len = vsnprintf(buf, len + 1, fmt, arg);
    }
    va_end(arg);
    push(buf, len);
    if (buf != loc) free(buf);
}

void logPrint(const char *s) { push(s, strlen(s)); }

void logPrintln(const char *s) { logPrintf("%s\r\n", s); }

void logPump() {
    if (s_ready) s_core.pump(s_out);
}

void logWaitRoom(size_t bytes) {
    if (!s_ready) return;
    // Com um descarte pendente, a mensagem seguinte também seria descartada: espera o aviso sair
    while (s_buf.freeBytes() < bytes || s_core.pendingMsgs() > 0) {
        logPump();
        delay(1);
    }
}

void logFlush() {
    if (s_ready) {
        // Até o buffer esvaziar e o aviso de um descarte pendente sair
        for (;;) {
            logPump();
            if (s_buf.waiting() == 0 && s_core.pendingMsgs() == 0) break;
            delay(1);
        }
    }
    Serial.flush();
}
