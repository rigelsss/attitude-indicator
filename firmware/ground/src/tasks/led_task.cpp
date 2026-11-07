// led_task.cpp — ver led_task.h
#include "led_task.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>
#include "alarm_task.h"
#include "config.h"
#include "link_task.h"
#include "ws2812_rmt.h"

static QueueHandle_t s_status = nullptr;
static LedPoint s_pts[LED_COUNT];
static LedRgb s_frame[LED_COUNT];

// Estado do anel, na prioridade da tabela do docs/caixa-b.md
static LedMode chooseMode(const LinkSnapshot &snap) {
    if (!snap.everLinked || !snap.linkOk || snap.dataLate) return LedMode::NoData;
    const TelemetryPacket &p = snap.item.pkt;
    if (p.status & ST_CALIBRATING) return LedMode::Calibrating;
    if (!(p.valid & VAL_ATT) || !isfinite(p.roll) || !isfinite(p.pitch)) return LedMode::AttInvalid;
    return LedMode::Horizon;
}

static void ledTask(void *) {
    const LedRingGeom top = {LED_TOP_DEPTH_MM, LED_TOP_WIDTH_MM, LED_TOP_N_NOSE, LED_TOP_N_RIGHT,
                             LED_TOP_N_TAIL,   LED_TOP_N_LEFT,   LED_TOP_START,  LED_TOP_CLOCKWISE};
    const LedRingGeom side = {LED_SIDE_DEPTH_MM, LED_SIDE_WIDTH_MM, LED_SIDE_N_NOSE, LED_SIDE_N_RIGHT,
                              LED_SIDE_N_TAIL,   LED_SIDE_N_LEFT,   LED_SIDE_START,  LED_SIDE_CLOCKWISE};
    ledRingLayout(top, s_pts, LED_TOP_COUNT);
    ledRingLayout(side, s_pts + LED_TOP_COUNT, LED_SIDE_COUNT);
    const LedRingSpan rings[2] = {{0, LED_TOP_COUNT, LED_TOP_GAIN}, {LED_TOP_COUNT, LED_SIDE_COUNT, LED_SIDE_GAIN}};
    const LedLook look = {LED_DEADBAND_DEG,   LED_SATURATION_DEG,  LED_COLOR_WHITE,     LED_COLOR_SKY,
                          LED_COLOR_GROUND,   LED_COLOR_AMBER,     LED_COLOR_ALARM,     LED_FLASH_ON_MS,
                          LED_FLASH_PERIOD_MS, LED_SPIN_PERIOD_MS, LED_PULSE_PERIOD_MS, LED_PULSE_MIN};
    const LedPowerConfig power = {LED_MA_PER_CHANNEL, LED_MA_IDLE, LED_MA_MAX};
    LedPot pot({4095, LED_POT_HYST, LED_POT_GAMMA, LED_BRIGHT_MIN});
    LedAlarmFlash flash;

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_LED_POT, ADC_11db);   // curso inteiro de 0 a 3,3 V
    // Sem o driver (memória ou erro do RMT), a task segue sem enviar: o c mostra "fita: ERRO"
    for (uint16_t i = 0; i < LED_COUNT; i++) s_frame[i] = {0, 0, 0};
    if (ws2812Begin(PIN_LED_STRIP, LED_RMT_CHANNEL, LED_COUNT, LED_COLOR_GRB)) ws2812Show(s_frame, LED_COUNT);

    LedStatus st = {};
    uint32_t lastRetryMs = millis();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t nowMs = millis();
        const LinkSnapshot snap = linkSnapshot((uint64_t)esp_timer_get_time());
        LedFrameInput in = {chooseMode(snap), 0.0f, 0.0f, false};
        AlarmStatus al = {};
        const bool haveAlarm = alarmCurrent(al);
        const bool attAlarm =
            haveAlarm && !al.acked && (al.kind == AlarmKind::BankCaution || al.kind == AlarmKind::AttitudeWarning);
        if (in.mode == LedMode::Horizon) {
            in.rollDeg = snap.item.pkt.roll;
            in.pitchDeg = snap.item.pkt.pitch;
        }
        // O flash conta a fase desde o início do alarme, mesmo nos quadros em que o anel mostra outro estado
        const bool flashOn = flash.update(attAlarm, (uint8_t)al.kind, nowMs, LED_FLASH_ON_MS, LED_FLASH_PERIOD_MS);
        in.flash = flashOn && in.mode == LedMode::Horizon;
        ledRenderFrame(look, s_pts, rings, 2, in, nowMs, s_frame);

        st.potRaw = (uint16_t)analogRead(PIN_LED_POT);
        st.brightness = pot.update(st.potRaw);
        st.potValue = pot.value();
        const LedPowerResult pr = ledApplyBrightness(s_frame, LED_COUNT, st.brightness, power);
        const int64_t t0 = esp_timer_get_time();
        // RMT: ~30 us por LED; a task espera o fim da transmissão (no máximo WS2812_TIMEOUT_MS), com a CPU livre
        // para as outras tasks. Prazo vencido: o driver recupera o canal (lib/ws2812_core) e o próximo quadro sai
        // normalmente. Se a reinstalação falhou, nova tentativa a cada LED_RETRY_MS; com o buffer preso, nunca
        if (ws2812State() == Ws2812State::NotInstalled && nowMs - lastRetryMs >= LED_RETRY_MS) {
            lastRetryMs = nowMs;
            ws2812Retry();
        }
        ws2812Show(s_frame, LED_COUNT);   // fora do estado "pronto", volta na hora sem tocar no buffer
        st.showUs = (uint32_t)(esp_timer_get_time() - t0);
        st.driver = ws2812State();
        st.failures = ws2812Failures();
        st.recoveries = ws2812Recoveries();
        st.recoveryFailures = ws2812RecoveryFailures();

        st.mode = in.mode;
        st.flash = in.flash;
        st.alarmActive = attAlarm;
        st.estimatedMa = pr.estimatedMa;
        st.frames++;
        if (pr.capped) st.cappedFrames++;
        xQueueOverwrite(s_status, &st);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / LED_RATE_HZ));
    }
}

bool ledTaskStart() {
    s_status = xQueueCreate(1, sizeof(LedStatus));
    if (s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(ledTask, "leds", LED_TASK_STACK, nullptr, LED_TASK_PRIO, nullptr, LED_TASK_CORE) ==
           pdPASS;
}

bool ledCurrent(LedStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
