// link_task.h — estado do link no ESP B (docs/radio.md, ESP B): consome a fila de recepção (radio_rx), conta
// perdas, duplicados e ressincronizações pelo seq e detecta o timeout (lib/link_stats). Publica:
//   - o último pacote, numa caixa de correio (fila de 1, xQueueOverwrite), para a serial e, nas semanas 4 e 5,
//     para o TFT e o WebSocket;
//   - o resumo de cada janela de LINK_REPORT_MS, noutra caixa de correio;
//   - os eventos do link (perdido, restabelecido, ressincronizado), numa fila: nenhum se perde em silêncio.
// Núcleo LINK_TASK_CORE (1), prioridade LINK_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>
#include "link_stats.h"
#include "radio_rx.h"

// Resumo de uma janela, com os totais desde o boot
struct LinkReport {
    uint32_t   index;      // resumos publicados desde o boot (a serial imprime cada um uma vez)
    uint32_t   windowMs;   // duração real da janela
    LinkState  state;
    LinkWindow win;
    LinkCounts totals;
    RxCounters rxWin, rxTotals;   // recusados pelo callback: na janela e desde o boot
    bool       havePacket;
    uint16_t   lastSeq;
    uint32_t   lastTms;    // t_ms do A no último pacote
    uint32_t   ageMs;      // idade do último pacote no fim da janela
    uint32_t   delayMs;    // atraso estimado do último pacote
    uint32_t   delayMaxMs; // maior atraso estimado na janela
};

struct LinkEventMsg {
    LinkUpdate u;
    uint32_t   atMs;   // millis() do B
};

// Último pacote aceito e o estado do dado num instante: a regra única de "dado atual", usada pelo alarme, pelo
// OLED e pelo TFT
struct LinkSnapshot {
    bool     everLinked;   // já chegou um pacote aceito (item vale)
    bool     linkOk;       // idade desde a chegada até LINK_TIMEOUT_MS (senão, link perdido)
    bool     dataLate;     // idade estimada do dado (atraso na chegada + tempo desde a chegada) acima de
                           // LINK_STALE_DELAY_MS
    uint32_t ageMs;        // tempo desde a chegada
    RxItem   item;
    bool current() const { return everLinked && linkOk && !dataLate; }
};

// Cria as filas e a task. false sem memória
bool linkTaskStart();
// Estado no instante nowUs (esp_timer_get_time()). A chegada é carimbada na task do Wi-Fi e pode ser um pouco
// posterior a nowUs: a idade fica em zero
LinkSnapshot linkSnapshot(uint64_t nowUs);
// Última cópia publicada. false se ainda não há
bool linkLatestPacket(RxItem &out);
bool linkLatestReport(LinkReport &out);
// Próximo evento do link, sem esperar. false se não há
bool linkEventReceive(LinkEventMsg &out);
// Eventos descartados com a fila cheia, desde o boot
uint32_t linkEventsDropped();
