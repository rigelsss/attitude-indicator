// link_stats.h — estado do link ESP-NOW no ESP B e contagem de perdas pelo seq (docs/radio.md, ESP B).
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Regras, a cada pacote válido (d = seq − último seq, em 16 bits: a volta de 65535 para 0 já sai certa):
//   - d == 0 (com o t_ms sem voltar): duplicado, contado e descartado;
//   - seq até lateMaxPkts atrás E t_ms até lateMaxMs atrás: atrasado (chegou depois de um mais novo), contado
//     e descartado. Sem identificador de boot no contrato, é o que separa um pacote velho de um A reiniciado;
//     no caso ambíguo (A reiniciado com até lateMaxPkts pacotes enviados), os primeiros pacotes do boot novo
//     são descartados como atrasados, e em até lateMaxPkts pacotes o fluxo volta a ser aceito (em ordem ou com
//     ressincronização), sem perda falsa;
//   - t_ms do A voltando além disso: o A reiniciou → ressincroniza, sem contar perdas;
//   - 1 ≤ d ≤ resyncGap: em ordem; conta d − 1 perdidos;
//   - d > resyncGap (inclui o seq voltando para trás): ressincroniza, sem contar perdas. Uma falta mais longa
//     que resyncGap pacotes também cai aqui, e as perdas dela não são contadas.
// Sem pacote por timeoutUs, o link fica perdido (poll); o pacote seguinte o restabelece. Se duas chegadas
// estiverem separadas por mais que timeoutUs sem o poll ter visto (task atrasada, pacotes esperando na fila),
// o onPacket registra a queda e o restabelecimento juntos.
// Só os pacotes aceitos (LinkUpdate::accepted) devem ser repassados aos consumidores.
// O intervalo entre chegadas (jitter visto no B) só é medido entre dois pacotes com o link já OK e sem
// ressincronização: inclui as lacunas de perdas isoladas (40 ms com um pacote perdido a 50 Hz). Nos mesmos
// pares, mede também o intervalo pelo t_ms do A, para separar o atraso do envio do atraso do ar e do B.
#pragma once
#include <stdint.h>

struct LinkConfig {
    uint64_t timeoutUs;   // sem pacote por mais que isto: link perdido
    uint16_t resyncGap;   // salto do seq acima disto: ressincroniza sem contar perdas
    uint16_t lateMaxPkts; // seq até isto atrás (e t_ms até lateMaxMs atrás): pacote atrasado, descartado
    uint32_t lateMaxMs;
};

enum class LinkState : uint8_t { NoSignal, Ok, Lost };
const char *linkStateText(LinkState s);

enum LinkEventBits : uint8_t {
    LINK_EV_FIRST    = 1 << 0,   // primeiro pacote desde o boot
    LINK_EV_RESTORED = 1 << 1,   // primeiro pacote depois de um timeout (outageUs)
    LINK_EV_RESYNC   = 1 << 2,   // A reiniciado ou salto do seq (seqFrom/seqTo, tFrom/tTo)
    LINK_EV_LOST     = 1 << 3,   // timeout: pelo poll, ou pelo onPacket junto com RESTORED (outageUs)
};

struct LinkUpdate {
    bool     accepted;   // pacote aceito (não duplicado nem atrasado): só esses vão aos consumidores
    uint8_t  events;     // LinkEventBits
    uint64_t outageUs;   // com RESTORED: tempo desde o último pacote
    uint16_t seqFrom, seqTo;
    uint32_t tFrom, tTo;   // t_ms do A
};

struct LinkCounts {
    uint32_t received;     // pacotes aceitos (sem os duplicados)
    uint32_t lost;         // lacunas do seq
    uint32_t duplicates;
    uint32_t late;         // atrasados (chegaram depois de um mais novo), descartados
    uint32_t resyncs;
    uint32_t timeouts;     // quedas do link
};

struct LinkWindow {
    LinkCounts counts;
    uint32_t   intervals;   // intervalos medidos
    uint32_t   intervalMinUs, intervalMaxUs;   // entre as chegadas no B
    uint64_t   intervalSumUs;
    // Os mesmos intervalos pelo t_ms do A (ms): se variam junto com os do B, o atraso está no envio do A; se
    // ficam em ~20 ms, está no ar ou na recepção
    uint32_t   srcIntervalMinMs, srcIntervalMaxMs;
};

class LinkStats {
public:
    explicit LinkStats(const LinkConfig &cfg);
    // Pacote válido. arrivalUs: instante da chegada (relógio monotônico, o mesmo do poll)
    LinkUpdate onPacket(uint16_t seq, uint32_t tMs, uint64_t arrivalUs);
    // Confere o timeout no instante nowUs
    LinkUpdate poll(uint64_t nowUs);
    // Devolve a janela desde a última chamada e a zera
    LinkWindow takeWindow();

    LinkState state() const { return state_; }
    const LinkCounts &totals() const { return totals_; }
    bool hasPacket() const { return state_ != LinkState::NoSignal; }
    uint16_t lastSeq() const { return lastSeq_; }
    uint32_t lastTms() const { return lastTms_; }
    uint64_t lastArrivalUs() const { return lastArrivalUs_; }

private:
    void count(uint32_t LinkCounts::*field, uint32_t n);
    void resetWindow();

    LinkConfig cfg_;
    LinkState  state_ = LinkState::NoSignal;
    uint16_t   lastSeq_ = 0;
    uint32_t   lastTms_ = 0;
    uint64_t   lastArrivalUs_ = 0;
    LinkCounts totals_ = {};
    LinkWindow win_ = {};
};
