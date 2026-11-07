// led_task.h — anel de LEDs da caixa do ESP B (docs/caixa-b.md, "Anel de LEDs"): a LED_RATE_HZ, lê o estado do
// link (linkSnapshot, a regra única de dado atual) e do alarme (alarmCurrent), escolhe o estado do anel, monta o
// quadro (lib/led_ring), aplica o brilho do potenciômetro e o teto de corrente e envia à fita pelo RMT.
// Núcleo LED_TASK_CORE (1), prioridade LED_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>
#include "led_ring.h"
#include "ws2812_core.h"

struct LedStatus {
    LedMode  mode;
    bool     flash;        // flash do alarme aceso no último quadro
    bool     alarmActive;  // alarme de atitude sem reconhecimento (o flash está em curso)
    uint16_t potRaw;       // última leitura do ADC
    uint16_t potValue;     // média com a histerese
    float    brightness;   // 0..1
    float    estimatedMa;  // corrente estimada do último quadro, depois do teto
    uint32_t frames;       // quadros enviados desde o boot
    uint32_t cappedFrames; // quadros reduzidos pelo teto
    uint32_t showUs;       // tempo do último envio à fita
    Ws2812State driver;    // estado atual do driver da fita (lib/ws2812_core), lido a cada quadro
    uint32_t failures;     // envios com prazo vencido ou erro, desde o boot
    uint32_t recoveries;   // recuperações do canal do RMT executadas
    uint32_t recoveryFailures;   // recuperações que não reinstalaram (ou com a desinstalação falhando)
};

// Cria a task (que inicia a fita e o ADC). false sem memória
bool ledTaskStart();
// Estado atual (cópia). false antes do primeiro quadro
bool ledCurrent(LedStatus &out);
