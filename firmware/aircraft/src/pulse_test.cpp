// pulse_test.cpp — ver pulse_test.h
#ifdef PULSE_TEST
#include "pulse_test.h"
#include <Arduino.h>
#include <atomic>
#include <esp_random.h>
#include <esp_timer.h>
#include "config.h"
#include "logger.h"
#include "tasks/sensor_task.h"

static esp_timer_handle_t s_timer = nullptr;
static std::atomic<bool> s_running{false};
static uint32_t s_endUs = 0;      // só o callback do timer usa depois do início
static uint32_t s_injected = 0;
static uint32_t s_queueFull = 0;  // injeções recusadas com a fila do INT cheia (só o callback do timer escreve)

static uint32_t nextDelayUs() {
    return PULSE_TEST_MIN_US + esp_random() % (PULSE_TEST_MAX_US - PULSE_TEST_MIN_US + 1);
}

// Roda na task do esp_timer (não é ISR): enfileira um instante falso e rearma com intervalo aleatório
static void onTimer(void *) {
    const uint32_t now = (uint32_t)esp_timer_get_time();
    if ((int32_t)(now - s_endUs) >= 0) {
        logPrintf("[teste] injeção terminada: %lu pulsos falsos (%lu recusados com a fila do INT cheia)\n",
                  (unsigned long)s_injected, (unsigned long)s_queueFull);
        s_running.store(false);
        return;
    }
    if (sensorInjectPulse(now)) s_injected++;
    else s_queueFull++;
    esp_timer_start_once(s_timer, nextDelayUs());
}

void pulseTestStart() {
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) return;
    if (s_timer == nullptr) {
        const esp_timer_create_args_t args = {onTimer, nullptr, ESP_TIMER_TASK, "pulse_test", false};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            logPrintln("[teste] ERRO: timer da injeção não criado");
            s_running.store(false);
            return;
        }
    }
    s_injected = 0;
    s_queueFull = 0;
    s_endUs = (uint32_t)esp_timer_get_time() + PULSE_TEST_MS * 1000UL;
    logPrintf("[teste] injetando pulsos falsos no INT por %d ms (intervalo de %d a %d µs)\n", PULSE_TEST_MS,
              PULSE_TEST_MIN_US, PULSE_TEST_MAX_US);
    esp_timer_start_once(s_timer, nextDelayUs());
}

#endif
