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
static bool s_ready = false;   // buffer criado; sem ele, modo degradado (ver push)
// Única task que esvazia o buffer e escreve no Serial (LogCore: um consumidor só), registrada por
// logAttachSerial() depois do Serial.begin(). Antes dela, as mensagens só se acumulam. Escrita uma vez, antes
// de liberar s_serialReady: quem passa por logWaitSerial() já a enxerga
static TaskHandle_t s_consumer = nullptr;
static uint32_t s_flushTimeoutMs = 0;

// Semáforos estáticos (sem alocação: valem também quando o heap falhou)
static StaticSemaphore_t s_serialReadyBuf, s_flushReqBuf, s_flushDoneBuf;
static SemaphoreHandle_t s_serialReady = nullptr;   // dado pelo logAttachSerial()
static SemaphoreHandle_t s_flushReq = nullptr;      // pedido de flush de outra task para a consumidora
static SemaphoreHandle_t s_flushDone = nullptr;     // flush concluído pela consumidora
static bool s_flushPending = false;                 // só a consumidora usa

static bool isConsumer() { return s_consumer != nullptr && xTaskGetCurrentTaskHandle() == s_consumer; }

// Modo degradado (sem buffer): antes do Serial pronto, a mensagem é descartada; depois, escrita direta, que
// bloqueia quem imprime (como antes da P2). O HardwareSerial tem mutex: a escrita é segura entre tasks
static void push(const char *s, size_t len) {
    if (len == 0) return;
    if (!s_ready) {
        if (s_consumer != nullptr) Serial.write((const uint8_t *)s, len);
        return;
    }
    s_core.push(s, len);
}

// Tudo o que entrou no buffer já foi escrito no Serial. Só a consumidora sabe: ela retira um trecho antes de
// escrevê-lo, e nesse intervalo o buffer parece vazio para as outras tasks
static bool drained() { return !s_ready || (s_buf.waiting() == 0 && s_core.pendingMsgs() == 0); }

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

bool logBegin(size_t bytes, uint32_t flushTimeoutMs) {
    s_flushTimeoutMs = flushTimeoutMs;
    s_serialReady = xSemaphoreCreateBinaryStatic(&s_serialReadyBuf);
    s_flushReq = xSemaphoreCreateBinaryStatic(&s_flushReqBuf);
    s_flushDone = xSemaphoreCreateBinaryStatic(&s_flushDoneBuf);
    s_buf.rb = xRingbufferCreate(bytes, RINGBUF_TYPE_BYTEBUF);
    if (s_buf.rb == nullptr) return false;
    s_ready = true;
    ets_install_putc1(corePutc);
    esp_log_set_vprintf(idfVprintf);
    return true;
}

void logAttachSerial() {
    if (s_ready) {
        // Desliga o canal de depuração do núcleo no Serial: sem ele, log_printf não espera a UART ficar
        // ociosa. setDebugOutput(false) também desinstala o putc do ets_printf, então ele é instalado de novo.
        // No modo degradado, o log do núcleo fica como no Arduino (direto na UART)
        Serial.setDebugOutput(false);
        ets_install_putc1(corePutc);
    }
    s_consumer = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_serialReady);
}

bool logWaitSerial(uint32_t timeoutMs) {
    if (s_serialReady == nullptr || xSemaphoreTake(s_serialReady, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
    xSemaphoreGive(s_serialReady);   // fica disponível para outra espera
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
    if (!isConsumer()) return;
    if (s_ready) s_core.pump(s_out);
    // Pedido de flush de outra task: concluído quando tudo o que entrou no buffer já foi escrito
    if (!s_flushPending && xSemaphoreTake(s_flushReq, 0) == pdTRUE) s_flushPending = true;
    if (s_flushPending && drained()) {
        Serial.flush();
        s_flushPending = false;
        xSemaphoreGive(s_flushDone);
    }
}

// Nas esperas, quem não é o consumidor só aguarda: a serial_task esvazia o buffer
void logWaitRoom(size_t bytes) {
    if (!s_ready || s_consumer == nullptr) return;
    // Com um descarte pendente, a mensagem seguinte também seria descartada: espera o aviso sair
    while (s_buf.freeBytes() < bytes || s_core.pendingMsgs() > 0) {
        logPump();
        delay(1);
    }
}

void logFlush() {
    if (s_consumer == nullptr) return;   // Serial ainda não iniciado: nada a esperar
    if (isConsumer()) {
        while (!drained()) {
            s_core.pump(s_out);
            delay(1);
        }
        Serial.flush();
        return;
    }
    // Outra task: a consumidora faz o flush e confirma. Com o limite, um reinício não trava se ela parar
    xSemaphoreTake(s_flushDone, 0);   // descarta uma confirmação antiga
    xSemaphoreGive(s_flushReq);
    xSemaphoreTake(s_flushDone, pdMS_TO_TICKS(s_flushTimeoutMs));
}
