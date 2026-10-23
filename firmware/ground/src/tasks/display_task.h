// display_task.h — horizonte artificial no TFT ST7789 240x240 do ESP B (docs/solo.md, passo 5).
// A cada 1/DISPLAY_FPS: lê o estado do link (linkSnapshot, a regra única de "dado atual") e o alarme, desenha
// num sprite de 8 bits (RGB332) e o envia ao TFT. Geometria em lib/pfd_geometry. Sem dado atual ou com a
// atitude inválida, mostra a bandeira de falha (fundo preto, X vermelho e o motivo), e nunca um horizonte parado.
// O TFT e o sprite são iniciados por displayInit() no setup(), antes do Wi-Fi (o sprite de 57,6 kB precisa de um
// bloco contíguo); depois, só a task os usa.
// Núcleo DISPLAY_TASK_CORE (1), prioridade DISPLAY_TASK_PRIO (config.h), abaixo da link_task.
#pragma once
#include <stdint.h>

struct DisplayStatus {
    bool     spriteOk;     // sprite alocado (sem ele, o TFT mostra só uma mensagem de erro)
    float    fps;          // quadros na última janela de 1 s
    uint32_t drawUs;       // desenho no sprite, média da janela
    uint32_t pushUs;       // envio ao TFT, média da janela
    uint32_t maxFrameUs;   // maior quadro (desenho + envio) da janela
    uint32_t frames;       // desde o boot
    uint32_t overruns;     // quadros mais longos que o período, desde o boot
};

// Inicia o TFT e aloca o sprite (chamar no setup(), antes do Wi-Fi). Imprime o resultado no Serial.
// false sem memória para o sprite (o TFT mostra a mensagem de erro; a task segue só publicando o estado)
bool displayInit();
// Cria a task. false sem memória
bool displayTaskStart();
// Estado atual (cópia). false antes da primeira janela
bool displayCurrent(DisplayStatus &out);
