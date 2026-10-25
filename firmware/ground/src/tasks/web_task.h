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
#include "config.h"

// Um cliente do WebSocket, para o c (qual aparelho perde dados)
struct WsClientStat {
    bool     on;        // conectado (handshake concluído)
    uint32_t ip;        // como o IPAddress guarda
    uint32_t sent;      // telemetrias enviadas desde a conexão
    uint32_t skipped;   // telemetrias puladas desde a conexão (quadro anterior saindo, fila pendente ou
                        // WS_MAX_INFLIGHT bytes sem confirmação)
    uint32_t idleMs;    // tempo desde o último progresso no envio
    int32_t  inFlight;     // bytes sem confirmação na última consulta (−1: conexão já não existe)
    int32_t  inFlightMax;  // maior valor desde a conexão
};

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
    uint32_t wsPartial;     // quadros que saíram pela metade: cliente derrubado (a biblioteca poderia escrever no meio)
    uint32_t wsDropped;     // clientes do WebSocket derrubados por WS_STALL_MS sem espaço
    uint32_t tcpResets;     // conexões encerradas com reset (derrubadas, erro no envio, HTTP ociosa)
    bool     heapPaused;    // telemetria pausada agora por heap baixo (WEB_HEAP_PAUSE_KB)
    uint32_t heapPauses;    // vezes que pausou
    uint32_t heapPausedMs;  // tempo total pausado
    WsClientStat ws[AP_MAX_CLIENTS];
};

// Eventos de rede para a serial (diagnóstico de vários celulares, 24/10/2026): quem entra e sai do AP, as conexões
// e requisições HTTP e os clientes do WebSocket, com o IP. O callback do Wi-Fi e a web_task só enfileiram; quem
// imprime é a serial_task
enum class NetEvKind : uint8_t {
    StaJoin,      // celular associado ao AP (mac)
    StaLeave,     // celular saiu do AP (mac)
    StaIp,        // DHCP entregou um IP (ip)
    HttpOpen,     // conexão HTTP aceita (slot, ip)
    HttpReq,      // requisição respondida (slot, ip, path, code)
    HttpIdle,     // fechada por HTTP_IDLE_MS (slot, ip, code: 0 esperando requisição, 1 enviando)
    HttpGone,     // o celular fechou antes de mandar a requisição (slot, ip)
    HttpEvict,    // ociosa fechada para dar lugar a outra (slot, ip)
    HttpReject,   // nova recusada: todas as vagas enviando (ip)
    WsOpen,       // cliente do WebSocket conectado (slot, ip)
    WsClose,      // cliente do WebSocket desconectado (slot)
    WsDrop,       // derrubado por WS_STALL_MS sem espaço (slot)
    HeapPause,    // telemetria pausada para todos por heap baixo (code: heap livre em kB)
    HeapResume,   // telemetria retomada (code: heap livre em kB; ip: ms pausado)
    WsSendErr,    // envio direto falhou e o cliente foi descartado (slot, code: errno; 0 = sem socket)
    WsPartial,    // quadro saiu pela metade (falta de memória no lwip): cliente derrubado (slot)
};

struct NetEventMsg {
    NetEvKind kind;
    uint8_t  slot;
    uint16_t code;
    uint32_t atMs;
    uint32_t ip;        // como o IPAddress guarda (IPAddress(ip) reconstrói)
    uint8_t  mac[6];
    char     path[24];  // HttpReq: caminho (cortado)
};

bool webNetEventReceive(NetEventMsg &out);
uint32_t webNetEventsDropped();

// Cria a task (que monta o LittleFS e inicia os servidores). false sem memória
bool webTaskStart();
// Estado atual (cópia). false antes da primeira volta
bool webCurrent(WebStatus &out);
