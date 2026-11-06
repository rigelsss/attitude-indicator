// oled_task.cpp — ver oled_task.h
#include "oled_task.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_timer.h>
#include "alarm_task.h"
#include "config.h"
#include "command_task.h"
#include "input_task.h"
#include "link_task.h"
#include "oled_view.h"

// O OLED 2 usa a classe do Wire1 da U8g2 (exige WIRE_INTERFACES_COUNT=2 no platformio.ini). Ela inicia o Wire1
// sem pinos: eles são passados antes, por setPins
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled1(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C s_oled2(U8G2_R0, U8X8_PIN_NONE);
static QueueHandle_t s_status = nullptr;

struct Oled {
    U8G2       &u8g2;
    TwoWire    &wire;
    int         sda, scl;
    OledScreen  screen;
    const char *bootLine;   // segunda linha da tela de boot
};

static Oled s_oleds[OLED_COUNT] = {
    {s_oled1, Wire, PIN_I2C_SDA, PIN_I2C_SCL, OledScreen::Data, "OLED 1: dados"},
    {s_oled2, Wire1, PIN_I2C1_SDA, PIN_I2C1_SCL, OledScreen::Link, "OLED 2: link"},
};

static const int LINE_H = 10;   // fonte 6x10: 6 linhas de 21 caracteres

static bool probe(TwoWire &w) {
    w.beginTransmission(ADDR_OLED_DATA);
    return w.endTransmission() == 0;
}

// Confere o endereço e inicia o display. O begin() da U8g2 chama begin() no barramento: ele é liberado antes,
// para não haver o aviso "Bus already started"
static bool tryStart(Oled &o, bool wireStarted) {
    if (wireStarted) o.wire.end();
    o.wire.begin(o.sda, o.scl, OLED_I2C_FREQ_HZ);
    const bool ok = probe(o.wire);
    o.wire.end();
    if (!ok) {
        o.wire.begin(o.sda, o.scl, OLED_I2C_FREQ_HZ);   // fica iniciado para a próxima conferência
        return false;
    }
    o.wire.setPins(o.sda, o.scl);   // o begin() sem pinos do Wire1 na U8g2 usa estes
    o.u8g2.setI2CAddress(ADDR_OLED_DATA << 1);   // a U8g2 usa o endereço de 8 bits
    o.u8g2.setBusClock(OLED_I2C_FREQ_HZ);
    o.u8g2.begin();
    o.u8g2.setFont(u8g2_font_6x10_tf);
    o.u8g2.setFontPosTop();
    return true;
}

static void bootScreen(Oled &o) {
    o.u8g2.clearBuffer();
    o.u8g2.drawStr(1, 16, "Horizonte - ESP B");
    o.u8g2.drawStr(1, 34, o.bootLine);
    o.u8g2.sendBuffer();
}

// Entradas comuns às duas telas; a tela, o alvo e o rodapé são de cada OLED
static void gather(OledInputs &in, const InputStatus *inp) {
    memset(&in, 0, sizeof(in));
    // A regra única de "dado atual" (link_task), a mesma do alarme e do TFT
    const LinkSnapshot snap = linkSnapshot((uint64_t)esp_timer_get_time());
    in.everLinked = snap.everLinked;
    if (in.everLinked) {
        in.pkt = snap.item.pkt;
        in.linkOk = snap.linkOk;
        in.delayMs = snap.item.delayMs;
        in.dataLate = snap.dataLate;
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
    in.zeroTarget = inp != nullptr ? inp->target : CMD_ZERO_ATTITUDE;
    in.headingTrue = OLED_HEADING_TRUE != 0;
    in.declinationDeg = MAG_DECLINATION_DEG;
}

// Rodapé da pressão longa (só nos dados): o que vai acontecer com o botão apertado e, depois, o andamento
static const char *encoderFooter(char *buf, int n, const InputStatus *inp, uint8_t target) {
    CmdEventMsg ce;
    const bool haveEv = commandLastFromEncoder(ce);
    return oledEncoderFooter(buf, n, inp != nullptr && inp->buttonHeld, target, haveEv, haveEv ? ce.ev.cmd : 0,
                             haveEv ? ce.ev.state : CmdState::Free, haveEv && ce.localFull,
                             haveEv ? millis() - ce.atMs : 0, OLED_CMD_FOOTER_MS);
}

static uint32_t draw(U8G2 &u8g2, const OledPage &p) {
    u8g2.clearBuffer();
    if (p.banner) {   // sem link: título em vídeo inverso
        u8g2.drawBox(0, 0, 128, LINE_H);
        u8g2.setDrawColor(0);
        u8g2.drawStr(1, 1, p.lines[0]);
        u8g2.setDrawColor(1);
    } else {
        u8g2.drawStr(1, 1, p.lines[0]);
        u8g2.drawHLine(0, LINE_H, 128);
    }
    for (int r = 1; r < OLED_ROWS; r++) u8g2.drawStr(1, r * LINE_H + 2, p.lines[r]);
    const uint32_t t0 = micros();
    u8g2.sendBuffer();
    return micros() - t0;
}

static void oledTask(void *) {
    OledStatus st = {};
    bool anyPresent = false;
    for (int i = 0; i < OLED_COUNT; i++) {
        st.d[i].present = tryStart(s_oleds[i], false);
        if (st.d[i].present) {
            bootScreen(s_oleds[i]);
            anyPresent = true;
        }
    }
    if (anyPresent) vTaskDelay(pdMS_TO_TICKS(OLED_BOOT_SCREEN_MS));
    xQueueOverwrite(s_status, &st);
    uint32_t lastDrawMs[OLED_COUNT] = {}, lastTryMs[OLED_COUNT];
    for (int i = 0; i < OLED_COUNT; i++) lastTryMs[i] = millis();
    int lastTarget = -1;   // alvo desenhado no OLED 1: a troca redesenha na hora
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(OLED_TASK_PERIOD_MS));
        InputStatus inpBuf;
        const InputStatus *inp = inputCurrent(inpBuf) ? &inpBuf : nullptr;
        const uint8_t target = inp != nullptr ? inp->target : CMD_ZERO_ATTITUDE;
        const uint32_t now = millis();
        bool changed = false, anyDue = false, due[OLED_COUNT] = {};
        // 1ª passada: novas tentativas dos ausentes e quais OLEDs redesenham nesta volta
        for (int i = 0; i < OLED_COUNT; i++) {
            OledDisplayStatus &ds = st.d[i];
            if (!ds.present) {
                if (now - lastTryMs[i] >= OLED_RETRY_MS) {
                    lastTryMs[i] = now;
                    ds.present = tryStart(s_oleds[i], true);
                    lastDrawMs[i] = now - 1000 / OLED_RATE_HZ;   // presente de novo: redesenha na próxima volta
                    if (i == 0) lastTarget = -1;
                    changed = true;
                }
                continue;
            }
            const bool targetChanged = s_oleds[i].screen == OledScreen::Data && target != lastTarget;
            due[i] = targetChanged || now - lastDrawMs[i] >= 1000 / OLED_RATE_HZ;
            anyDue = anyDue || due[i];
        }
        // 2ª passada: um só retrato das cópias publicadas (link, resumo, alarme) para os OLEDs desta volta, só
        // quando algum vai redesenhar
        if (anyDue) {
            OledInputs common;
            gather(common, inp);
            for (int i = 0; i < OLED_COUNT; i++) {
                if (!due[i]) continue;
                Oled &o = s_oleds[i];
                OledDisplayStatus &ds = st.d[i];
                // O sendBuffer não informa erro: a presença é conferida antes de cada quadro
                if (!probe(o.wire)) {
                    ds.present = false;
                    lastTryMs[i] = now;
                    changed = true;
                    continue;
                }
                OledInputs in = common;
                in.screen = o.screen;
                static char footer[OLED_COLS + 1];
                if (o.screen == OledScreen::Data) {
                    in.footer = encoderFooter(footer, sizeof(footer), inp, in.zeroTarget);
                    lastTarget = in.zeroTarget;
                } else {
                    in.zeroTarget = 0;   // a marca é só dos dados
                }
                OledPage page;
                oledBuildPage(in, page);
                ds.frameUs = draw(o.u8g2, page);
                ds.frames++;
                lastDrawMs[i] = now;
                changed = true;
            }
        }
        if (changed) xQueueOverwrite(s_status, &st);
    }
}

bool oledTaskStart() {
    s_status = xQueueCreate(1, sizeof(OledStatus));
    if (s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(oledTask, "oled", OLED_TASK_STACK, nullptr, OLED_TASK_PRIO, nullptr,
                                   OLED_TASK_CORE) == pdPASS;
}

bool oledCurrent(OledStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
