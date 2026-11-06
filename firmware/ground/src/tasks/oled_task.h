// oled_task.h — os dois OLEDs de 0,96" do ESP B, uma tela fixa em cada (docs/solo.md, passo 3; docs/caixa-b.md):
// OLED 1 no Wire (dados, com a marca do alvo da pressão longa e o rodapé do comando) e OLED 2 no Wire1 (link e
// saúde do A). A task é a única dona dos dois barramentos (cada OLED é o único dispositivo no seu). A cada
// OLED_TASK_PERIOD_MS lê as cópias publicadas (último pacote e resumo da link_task, alarme atual, alvo da
// input_task) e redesenha cada OLED a OLED_RATE_HZ; o OLED 1 também logo que o alvo muda. O texto vem do
// lib/oled_view. Cada OLED confere o endereço antes de iniciar e antes de cada quadro: ausente, tenta de novo a
// cada OLED_RETRY_MS, sem afetar o outro.
// Núcleo OLED_TASK_CORE (0), prioridade OLED_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>

static const int OLED_COUNT = 2;   // 0 = OLED 1 (dados, Wire); 1 = OLED 2 (link, Wire1)

struct OledDisplayStatus {
    bool     present;   // respondeu no último quadro (ou na última tentativa)
    uint32_t frameUs;   // duração do último sendBuffer
    uint32_t frames;    // quadros enviados desde o boot
};

struct OledStatus {
    OledDisplayStatus d[OLED_COUNT];
};

// Cria a task. false sem memória
bool oledTaskStart();
// Estado atual (cópia). false antes da primeira volta da task
bool oledCurrent(OledStatus &out);
