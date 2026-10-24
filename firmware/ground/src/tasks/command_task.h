// command_task.h — comandos do ESP B para o ESP A (docs/web.md, passo 2). Recebe os pedidos (console, pressão
// longa do encoder e, no passo 3, a página), envia o CommandPacket pelo ESP-NOW com reenvio, casa os ACKs
// (lib/command_link) e publica cada mudança de estado (enviado, aceito, concluído, recusado, sem resposta, sem
// conclusão) para a serial. O seq do primeiro comando é aleatório a cada boot.
// Núcleo CMD_TASK_CORE (0), prioridade CMD_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>
#include "command_link.h"

enum CmdOrigin : uint8_t { CMD_FROM_CONSOLE = 0, CMD_FROM_ENCODER = 1, CMD_FROM_WEB = 2 };
const char *cmdOriginText(uint8_t origin);

struct CmdEventMsg {
    CmdEvent ev;
    uint8_t  attempt;    // com Sending: número do envio (1 = primeiro; acima disso, reenvio)
    bool     localFull;  // recusado no próprio B: todos os lugares ocupados por comandos em curso
    uint32_t atMs;       // millis() do B
};

struct CmdStats {
    uint32_t submitted, sends, sendErrors, acksMatched, acksIgnored, localRejects;
    uint32_t polls;   // reconsultas de comandos aceitos (incluídas em sends)
};

// Cria as filas e a task. false sem memória
bool commandTaskStart();
// Pede um comando (de qualquer task). false com a fila de pedidos cheia
bool commandSubmit(uint8_t cmd, int32_t arg, uint8_t origin);
// Próxima mudança de estado, sem esperar. false se não há
bool commandEventReceive(CmdEventMsg &out);
uint32_t commandEventsDropped();
// Última mudança de estado de um comando pedido pelo encoder (cópia). false se ainda não houve
bool commandLastFromEncoder(CmdEventMsg &out);
CmdStats commandStats();
