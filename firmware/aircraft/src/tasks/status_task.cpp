// status_task.cpp — ver status_task.h
#include "status_task.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <string.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "config.h"
#include "logger.h"
#include "radio_task.h"
#include "sensor_task.h"
#include "status_view.h"

static const int OLED_W = 128;
static const int OLED_LINE_H = 8;   // 4 linhas de 8 px; fonte u8g2_font_NokiaSmallPlain_tf (proporcional, traço fino, com acentos)
static const uint32_t OLED_FRAME_MS = 1000 / OLED_RATE_HZ;

static const StatusViewParams VIEW_PARAMS = {OLED_WARN_ROTATE_MS, OLED_SPURIOUS_SHOW_MS, LED_LINK_ON_MS,
                                             LED_LINK_PERIOD_MS, LED_IDLE_ON_MS, LED_IDLE_PERIOD_MS};

static portMUX_TYPE g_countersMux = portMUX_INITIALIZER_UNLOCKED;
static StatusCounters g_counters = {};

// ---------- OLED no Wire1 ----------

// Transferência do U8g2 pelo Wire1. O U8G2_..._2ND_HW_I2C do U8g2 não serve: neste framework o
// WIRE_INTERFACES_COUNT não é definido, e o driver dele compila sem enviar nada. Este também guarda o resultado
// de cada transação: um NACK no meio do quadro é o OLED solto
static bool g_xferFail = false;

static uint8_t oledByteWire1(u8x8_t *u8x8, uint8_t msg, uint8_t argInt, void *argPtr) {
    switch (msg) {
    case U8X8_MSG_BYTE_SEND:
        Wire1.write((const uint8_t *)argPtr, argInt);
        break;
    case U8X8_MSG_BYTE_INIT:     // o Wire1 já foi iniciado pela task
    case U8X8_MSG_BYTE_SET_DC:   // só no SPI
        break;
    case U8X8_MSG_BYTE_START_TRANSFER:
        Wire1.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (Wire1.endTransmission() != 0) g_xferFail = true;
        break;
    default:
        return 0;
    }
    return 1;
}

class StatusOled : public U8G2 {
public:
    StatusOled() : U8G2() {
        u8g2_Setup_ssd1306_i2c_128x32_univision_f(&u8g2, U8G2_R0, oledByteWire1, u8x8_gpio_and_delay_arduino);
        u8x8_SetI2CAddress(getU8x8(), ADDR_OLED_STATUS << 1);
    }
};

static StatusOled g_oled;   // o buffer do quadro (512 bytes) fica aqui, fora da pilha

static bool oledProbe() {
    Wire1.beginTransmission(ADDR_OLED_STATUS);
    return Wire1.endTransmission() == 0;
}

// Sequência de início, tela limpa e display ligado. false se alguma transação falhou
static bool oledInit() {
    g_xferFail = false;
    g_oled.begin();
    g_oled.setFlipMode(OLED_FLIP);
    g_oled.setFont(u8g2_font_NokiaSmallPlain_tf);
    g_oled.setFontPosTop();
    g_oled.setFontMode(1);
    return !g_xferFail;
}

// Largura (px) dos primeiros cols caracteres de s, na fonte atual (proporcional: depende das letras)
static int prefixWidth(const char *s, int cols) {
    char buf[STATUS_LINE_BYTES];
    size_t i = 0;
    for (int c = 0; s[i] != '\0' && i < sizeof(buf) - 1; i++) {
        if (((uint8_t)s[i] & 0xC0) == 0x80) continue;   // continuação de um caractere UTF-8
        if (c == cols) break;
        c++;
    }
    memcpy(buf, s, i);
    buf[i] = '\0';
    return g_oled.getUTF8Width(buf);
}

// Desenha a tela e envia o quadro; devolve o tempo do envio (µs), ou 0 se alguma transação falhou
static uint32_t oledDraw(const StatusScreen &s) {
    static bool warnedWide[STATUS_LINES] = {};
    g_oled.clearBuffer();
    g_oled.setDrawColor(1);
    for (int l = 0; l < STATUS_LINES; l++) {
        g_oled.drawUTF8(0, l * OLED_LINE_H, s.line[l]);
        if (!warnedWide[l] && g_oled.getUTF8Width(s.line[l]) > OLED_W) {
            warnedWide[l] = true;
            logPrintf("[status] AVISO: linha %d do OLED passa de %d px e fica cortada: \"%s\"\n", l + 1, OLED_W,
                      s.line[l]);
        }
    }
    // Vídeo inverso: caixas em XOR sobre os caracteres marcados
    g_oled.setDrawColor(2);
    for (int l = 0; l < STATUS_LINES; l++)
        for (int c = 0; c < STATUS_COLS;) {
            if (!(s.inverse[l] & (1UL << c))) {
                c++;
                continue;
            }
            int n = 0;
            while (c + n < STATUS_COLS && (s.inverse[l] & (1UL << (c + n)))) n++;
            const int x0 = prefixWidth(s.line[l], c);
            const int x1 = prefixWidth(s.line[l], c + n);
            g_oled.drawBox(x0, l * OLED_LINE_H, x1 - x0, OLED_LINE_H);
            c += n;
        }
    g_oled.setDrawColor(1);
    g_xferFail = false;
    const int64_t t0 = esp_timer_get_time();
    g_oled.sendBuffer();
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    return g_xferFail ? 0 : (us > 0 ? us : 1);
}

// ---------- Contadores ----------

static void notePresent(bool present, bool reinit, bool absence) {
    portENTER_CRITICAL(&g_countersMux);
    g_counters.present = present;
    if (reinit) g_counters.reinits++;
    if (absence) g_counters.absences++;
    portEXIT_CRITICAL(&g_countersMux);
}

static void noteFrame(uint32_t us) {
    portENTER_CRITICAL(&g_countersMux);
    StatusCounters &c = g_counters;
    c.frames++;
    if (c.frames == 1 || us < c.frameUsMin) c.frameUsMin = us;
    if (us > c.frameUsMax) c.frameUsMax = us;
    c.frameUsMean += ((float)us - c.frameUsMean) / (float)c.frames;
    portEXIT_CRITICAL(&g_countersMux);
}

StatusCounters statusCounters() {
    portENTER_CRITICAL(&g_countersMux);
    const StatusCounters c = g_counters;
    portEXIT_CRITICAL(&g_countersMux);
    return c;
}

// ---------- Entradas da tela ----------

static ResetKind resetKind(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_BROWNOUT: return ResetKind::Brownout;
    case ESP_RST_PANIC:    return ResetKind::Panic;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return ResetKind::Watchdog;
    default:               return ResetKind::Normal;
    }
}

static StatusInputs readInputs(ResetKind reset) {
    StatusInputs in = {};
    TelemetryState st;
    in.haveState = sensorLatestState(st);
    if (in.haveState) {
        in.mpuOk = st.mpuOk;
        in.baroOk = st.baroOk;
        in.magOk = st.magOk;
        in.calibrating = st.calibrating;
        in.intSpurious = st.intSpurious;
    }
    const RadioCounters rc = radioCounters();
    in.radio = rc.active ? RadioState::Active : rc.starting ? RadioState::Starting : RadioState::Off;
    in.sent = rc.sent;
    in.delivered = rc.macOk;
    in.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000);
    in.lastReset = reset;
    return in;
}

// ---------- Task ----------

static void statusTask(void *) {
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_STATUS, LOW);
    Wire1.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, I2C1_FREQ_HZ);   // nesta task: a ISR do I2C1 fica no núcleo dela
    Wire1.setTimeOut(I2C1_TIMEOUT_MS);
    const ResetKind reset = resetKind(esp_reset_reason());

    StatusView view(VIEW_PARAMS);
    StatusScreen screen;
    bool present = false, everPresent = false, warnedAbsent = false;
    uint32_t nextTryMs = 0, nextFrameMs = millis();
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_TASK_PERIOD_MS));
        const uint32_t now = millis();
        view.update(readInputs(reset), now);
        digitalWrite(PIN_LED_STATUS, view.ledOn(now) ? HIGH : LOW);

        if ((int32_t)(now - nextFrameMs) < 0) continue;
        nextFrameMs += OLED_FRAME_MS;
        if ((int32_t)(now - nextFrameMs) >= 0) nextFrameMs = now + OLED_FRAME_MS;   // atrasou: sem rajada

        if (!present) {
            if ((int32_t)(now - nextTryMs) < 0) continue;
            nextTryMs = now + OLED_RETRY_MS;
            if (!oledProbe() || !oledInit()) {
                if (!warnedAbsent && !everPresent)
                    logPrintf("[status] OLED ausente no Wire1 (0x%02X, SDA %d, SCL %d); nova tentativa a cada %d s\n",
                              ADDR_OLED_STATUS, PIN_I2C1_SDA, PIN_I2C1_SCL, OLED_RETRY_MS / 1000);
                warnedAbsent = true;
                continue;
            }
            present = true;
            if (everPresent) logPrintln("[status] OLED de volta: reiniciado");
            else
                logPrintf("[status] OLED iniciado no Wire1 (0x%02X, SDA %d, SCL %d, %lu kHz), %d quadros/s\n",
                          ADDR_OLED_STATUS, PIN_I2C1_SDA, PIN_I2C1_SCL, (unsigned long)(I2C1_FREQ_HZ / 1000),
                          OLED_RATE_HZ);
            notePresent(true, everPresent, false);
            everPresent = true;
        }

        view.render(now, screen);
        const uint32_t us = oledDraw(screen);
        if (us == 0) {   // solto no meio do quadro
            present = false;
            nextTryMs = now + OLED_RETRY_MS;
            notePresent(false, false, true);
            logPrintf("[status] OLED parou de responder; nova tentativa a cada %d s\n", OLED_RETRY_MS / 1000);
            continue;
        }
        noteFrame(us);
    }
}

bool statusTaskStart() {
    const bool ok = xTaskCreatePinnedToCore(statusTask, "status", STATUS_TASK_STACK, nullptr, STATUS_TASK_PRIO,
                                            nullptr, STATUS_TASK_CORE) == pdPASS;
    portENTER_CRITICAL(&g_countersMux);
    g_counters.started = ok;
    portEXIT_CRITICAL(&g_countersMux);
    return ok;
}
