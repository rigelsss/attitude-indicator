// oled_task.h — OLED de 0,96" do ESP B (docs/solo.md, passo 3): única dona do Wire (o OLED é o único
// dispositivo no I2C do B). A cada OLED_TASK_PERIOD_MS lê as cópias publicadas (último pacote e resumo da
// link_task, alarme atual, página da input_task) e redesenha a OLED_RATE_HZ ou logo que a página muda. O texto
// das páginas vem do lib/oled_view. Confere o endereço antes de iniciar e antes de cada quadro: ausente, tenta
// de novo a cada OLED_RETRY_MS.
// Núcleo OLED_TASK_CORE (0), prioridade OLED_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>

struct OledStatus {
    bool     present;   // respondeu no último quadro (ou na última tentativa)
    uint32_t frameUs;   // duração do último sendBuffer
    uint32_t frames;    // quadros enviados desde o boot
    uint8_t  page;      // página exibida
};

// Cria a task. false sem memória
bool oledTaskStart();
// Estado atual (cópia). false antes da primeira volta da task
bool oledCurrent(OledStatus &out);
