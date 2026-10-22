// status_task.h — OLED de status 0,91" (SSD1306 128x32, ADDR_OLED_STATUS) no segundo barramento I2C (Wire1,
// PIN_I2C1_SDA/SCL), separado dos sensores para um quadro (~13 ms) não atrasar o MPU, e LED de status
// (PIN_LED_STATUS). Lê só cópias: o estado publicado pela sensor_task e os contadores do rádio. A tela e o
// padrão do LED vêm do lib/status_view. OLED ausente ou solto: nova tentativa a cada OLED_RETRY_MS, sem afetar
// o resto. Núcleo STATUS_TASK_CORE (0), prioridade STATUS_TASK_PRIO (config.h). docs/tasks.md, passo 6.
#pragma once
#include <stdint.h>

// Contadores do OLED desde o boot (cópia consistente, tirada numa seção crítica)
struct StatusCounters {
    bool     started;       // task criada
    bool     present;       // OLED respondendo agora
    uint32_t frames;        // quadros enviados
    uint32_t frameUsMin, frameUsMax;
    float    frameUsMean;   // tempo do envio de um quadro (sendBuffer), µs
    uint32_t absences;      // vezes em que o OLED parou de responder
    uint32_t reinits;       // vezes em que voltou e foi reiniciado
};

// Cria a task (que inicia o Wire1 no próprio núcleo). false sem memória
bool statusTaskStart();
StatusCounters statusCounters();
