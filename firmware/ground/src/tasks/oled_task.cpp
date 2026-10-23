// oled_task.cpp — ver oled_task.h
#include "oled_task.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_timer.h>
#include "alarm_task.h"
#include "config.h"
#include "input_task.h"
#include "delay_estimator.h"
#include "link_task.h"
#include "oled_view.h"

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static QueueHandle_t s_status = nullptr;

static const int LINE_H = 10;   // fonte 6x10: 6 linhas de 21 caracteres

static bool probe() {
    Wire.beginTransmission(ADDR_OLED_DATA);
    return Wire.endTransmission() == 0;
}

// Confere o endereço e inicia o display. O begin() da U8g2 chama Wire.begin() com os pinos do construtor: o
// barramento é liberado antes, para não haver o aviso "Bus already started"
static bool tryStart(bool wireStarted) {
    if (wireStarted) Wire.end();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_FREQ_HZ);
    const bool ok = probe();
    Wire.end();
    if (!ok) {
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_FREQ_HZ);   // fica iniciado para a próxima conferência
        return false;
    }
    s_oled.setI2CAddress(ADDR_OLED_DATA << 1);   // a U8g2 usa o endereço de 8 bits
    s_oled.setBusClock(OLED_I2C_FREQ_HZ);
    s_oled.begin();
    s_oled.setFont(u8g2_font_6x10_tf);
    s_oled.setFontPosTop();
    return true;
}

static void gather(OledInputs &in) {
    memset(&in, 0, sizeof(in));
    RxItem last;
    in.everLinked = linkLatestPacket(last);
    if (in.everLinked) {
        in.pkt = last.pkt;
        const uint64_t nowUs = (uint64_t)esp_timer_get_time();
        const uint64_t ageUs = nowUs > last.arrivalUs ? nowUs - last.arrivalUs : 0;
        in.linkOk = ageUs <= (uint64_t)LINK_TIMEOUT_MS * 1000;   // a mesma regra do timeout da link_task
        in.delayMs = last.delayMs;
        in.dataLate = dataAgeMs(last.delayMs, ageUs / 1000) > LINK_STALE_DELAY_MS;   // a mesma regra do alarme
    }
    LinkReport r;
    if (linkLatestReport(r)) {
        OledLinkStats &l = in.link;
        l.valid = true;
        l.recPerS = r.windowMs > 0 ? (r.win.counts.received * 1000 + r.windowMs / 2) / r.windowMs : 0;
        l.lostTotal = r.totals.lost;
        const uint32_t expected = r.totals.received + r.totals.lost;
        l.lossPct = expected > 0 ? 100.0f * r.totals.lost / expected : 0.0f;
        l.timeouts = r.totals.timeouts;
        l.haveIntervals = r.win.intervals > 0;
        if (l.haveIntervals) {
            l.intMinMs = r.win.intervalMinUs / 1000.0f;
            l.intAvgMs = (float)r.win.intervalSumUs / r.win.intervals / 1000.0f;
            l.intMaxMs = r.win.intervalMaxUs / 1000.0f;
        }
    }
    AlarmStatus al;
    in.alarmText = alarmCurrent(al) ? alarmKindShort(al.kind) : "---";
    InputStatus inp;
    in.page = inputCurrent(inp) ? inp.page : 0;
    in.pages = OLED_PAGES;
    in.headingTrue = OLED_HEADING_TRUE != 0;
    in.declinationDeg = MAG_DECLINATION_DEG;
}

static uint32_t draw(const OledPage &p) {
    s_oled.clearBuffer();
    if (p.banner) {   // sem link: título em vídeo inverso
        s_oled.drawBox(0, 0, 128, LINE_H);
        s_oled.setDrawColor(0);
        s_oled.drawStr(1, 1, p.lines[0]);
        s_oled.setDrawColor(1);
    } else {
        s_oled.drawStr(1, 1, p.lines[0]);
        s_oled.drawHLine(0, LINE_H, 128);
    }
    for (int r = 1; r < OLED_ROWS; r++) s_oled.drawStr(1, r * LINE_H + 2, p.lines[r]);
    const uint32_t t0 = micros();
    s_oled.sendBuffer();
    return micros() - t0;
}

static void oledTask(void *) {
    OledStatus st = {};
    st.present = tryStart(false);
    if (st.present) {
        s_oled.clearBuffer();
        s_oled.drawStr(1, 16, "Horizonte - ESP B");
        s_oled.drawStr(1, 34, "aguardando o A");
        s_oled.sendBuffer();
        vTaskDelay(pdMS_TO_TICKS(OLED_BOOT_SCREEN_MS));
    }
    xQueueOverwrite(s_status, &st);
    uint32_t lastDrawMs = 0, lastTryMs = millis();
    int lastPage = -1;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(OLED_TASK_PERIOD_MS));
        const uint32_t now = millis();
        if (!st.present) {
            if (now - lastTryMs >= OLED_RETRY_MS) {
                lastTryMs = now;
                st.present = tryStart(true);
                lastPage = -1;   // redesenha na hora
                xQueueOverwrite(s_status, &st);
            }
            continue;
        }
        OledInputs in;
        gather(in);
        if (in.page == lastPage && now - lastDrawMs < 1000 / OLED_RATE_HZ) continue;
        // O sendBuffer não informa erro: a presença é conferida antes de cada quadro
        if (!probe()) {
            st.present = false;
            lastTryMs = now;
            xQueueOverwrite(s_status, &st);
            continue;
        }
        OledPage page;
        oledBuildPage(in, page);
        st.frameUs = draw(page);
        st.frames++;
        st.page = in.page;
        lastPage = in.page;
        lastDrawMs = now;
        xQueueOverwrite(s_status, &st);
    }
}

bool oledTaskStart() {
    s_status = xQueueCreate(1, sizeof(OledStatus));
    if (s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(oledTask, "oled", OLED_TASK_STACK, nullptr, OLED_TASK_PRIO, nullptr,
                                   OLED_TASK_CORE) == pdPASS;
}

bool oledCurrent(OledStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
