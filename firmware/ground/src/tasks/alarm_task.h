// alarm_task.h — alarmes do ESP B (docs/solo.md, passo 1): a cada ALARM_TASK_PERIOD_MS, lê o último pacote
// aceito da link_task (caixa de correio) e a hora de chegada, avalia as condições (lib/alarm_logic) e aciona o
// LED de alarme (PIN_LED_ALARM), o LED do link (PIN_LED_LINK) e, com ALARM_BUZZER_ENABLED, o buzzer. As
// mudanças de alarme vão para a serial por uma fila. O reconhecimento chega por alarmAcknowledge() (tecla k
// agora; botão do encoder no passo 2).
// Núcleo ALARM_TASK_CORE (0), prioridade ALARM_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>
#include "alarm_logic.h"

struct AlarmEventMsg {
    enum Type : uint8_t { Changed, Acked, NothingToAck } type;
    AlarmKind kind;        // condição nova (Changed) ou reconhecida (Acked)
    float     rollDeg, pitchDeg;
    uint16_t  valid, status;
    uint32_t  atMs;        // millis() do B
};

struct AlarmStatus {
    AlarmKind kind;
    bool      acked;
    bool      linkLed;
};

// Cria as filas e a task (que configura os GPIOs). false sem memória
bool alarmTaskStart();
// Pede o reconhecimento do alarme ativo (de qualquer task). false se a fila estiver cheia
bool alarmAcknowledge();
// Próxima mudança de alarme, sem esperar. false se não há
bool alarmEventReceive(AlarmEventMsg &out);
// Mudanças de alarme descartadas com a fila cheia, desde o boot
uint32_t alarmEventsDropped();
// Estado atual (cópia). false antes da primeira volta da task
bool alarmCurrent(AlarmStatus &out);
