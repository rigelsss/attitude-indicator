// web_task.h — servidor web do ESP B (docs/web.md, passo 3), no AP "Horizonte-ESP" (192.168.4.1):
//   - HTTP na porta HTTP_PORT: os arquivos da página, do LittleFS (pio run -e ground -t uploadfs), por um servidor
//     próprio que atende até HTTP_MAX_CONNS conexões ao mesmo tempo sem travar (lib/http_static);
//   - WebSocket na porta WS_PORT: "hello" na conexão, telemetria ("tel") a WS_RATE_HZ com clientes conectados,
//     andamento dos comandos ("cmd") para todos, e os comandos recebidos, que exigem o WEB_CMD_PIN (lib/web_json)
//     e vão para a command_task com a origem "página".
// A telemetria segue a regra única de "dado atual" (linkSnapshot), a mesma do alarme, do OLED e do TFT.
// Núcleo WEB_TASK_CORE (0), prioridade WEB_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>

struct WebStatus {
    bool     fsOk;          // LittleFS montado e com o index.html
    uint8_t  clients;       // clientes do WebSocket conectados
    uint32_t telSent;       // mensagens de telemetria enviadas (difusão), desde o boot
    uint32_t cmdsOk;        // comandos da página aceitos pela fila da command_task
    uint32_t cmdsRefused;   // recusados no B (PIN, JSON, comando desconhecido, fila cheia)
    uint32_t httpReqs;      // requisições HTTP atendidas
    uint8_t  httpOpen;      // conexões HTTP abertas agora (até HTTP_MAX_CONNS)
    uint32_t httpIdle;      // fechadas por HTTP_IDLE_MS sem requisição ou sem progresso
    uint32_t httpEvicted;   // ociosas fechadas para dar lugar a uma nova (todas as vagas ocupadas)
    uint32_t httpRejected;  // novas recusadas (todas as vagas enviando)
    uint32_t wsSkipped;     // telemetrias não enviadas a um cliente (sem espaço ou com a fila pendente)
    uint32_t wsResyncs;     // filas refeitas a partir dos comandos recentes por terem enchido (WS_OUTBOX_LEN)
    uint32_t wsSendFail;    // sendTXT recusado (a mensagem da fila espera a próxima volta)
    uint32_t wsDropped;     // clientes do WebSocket derrubados por WS_STALL_MS sem espaço
};

// Cria a task (que monta o LittleFS e inicia os servidores). false sem memória
bool webTaskStart();
// Estado atual (cópia). false antes da primeira volta
bool webCurrent(WebStatus &out);
