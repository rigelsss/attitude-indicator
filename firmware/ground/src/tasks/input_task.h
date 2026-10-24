// input_task.h — encoder KY-040 do ESP B (docs/solo.md, passo 2).
// Giro: uma ISR em IRAM, registrada pelo ESP-IDF, lê CLK e DT de uma vez a cada borda e só enfileira o estado
// (com a fila cheia, descarta e enfileira depois um marcador de lacuna, onde a task ressincroniza);
// a task decodifica os cliques (lib/encoder_decode) e troca a página do OLED (horário = próxima). Botão: lido
// pela task a cada INPUT_TASK_WAIT_MS, com debounce; curto = reconhecer o alarme (alarmAcknowledge, como a
// tecla k), longo = comando conforme a página do OLED (encoderLongPressCommand: dados = zerar a atitude, link =
// zerar a altitude), pela command_task (docs/web.md).
// Núcleo INPUT_TASK_CORE (0), prioridade INPUT_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>

struct InputEventMsg {
    // AckDropped: fila do alarme cheia; CmdDropped: fila de comandos cheia (pressão longa perdida)
    enum Type : uint8_t { Click, ShortPress, LongPress, AckDropped, CmdDropped } type;
    int8_t   dir;    // Click: +1 horário, −1 anti-horário; LongPress e CmdDropped: o comando (CommandId)
    uint8_t  page;   // página depois do clique (0..OLED_PAGES−1)
    uint32_t atMs;   // millis() do B
};

struct InputStatus {
    uint8_t  page;
    uint32_t clicksCw, clicksCcw;   // desde o boot
    uint32_t invalid;               // transições que pularam um estado
    uint32_t isrDropped;            // estados descartados com a fila da ISR cheia
    uint32_t gaps;                  // lacunas (marcadores) tratadas: clique parcial descartado e ressincronização
    bool     buttonHeld;            // botão apertado agora (o OLED mostra o que a pressão longa vai fazer)
};

// Cria as filas e a task (que configura os pinos e registra a ISR no próprio núcleo) e espera essa
// configuração. false sem memória ou se a ISR não for registrada
bool inputTaskStart();
// Comando da pressão longa na página do OLED: 0 (dados) = CMD_ZERO_ATTITUDE; as outras = CMD_ZERO_ALTITUDE
uint8_t encoderLongPressCommand(uint8_t page);
// Próximo evento, sem esperar. false se não há
bool inputEventReceive(InputEventMsg &out);
// Eventos descartados com a fila cheia, desde o boot
uint32_t inputEventsDropped();
// Estado atual (cópia), inclusive a página do OLED. false antes da primeira volta da task
bool inputCurrent(InputStatus &out);
