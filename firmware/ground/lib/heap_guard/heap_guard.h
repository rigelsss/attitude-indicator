// heap_guard.h — trava de memória do WebSocket no ESP B (docs/cliente-python.md, bancadas 1 e 2; docs/plano.md,
// "Pendências da semana 6"). Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Com o heap livre abaixo de pauseKb, a primeira medida é derrubar o culpado: o cliente com algo pendente (quadro
// por sair ou bytes sem confirmação) e sem progresso no envio há pelo menos stallMs, o mais parado deles. Um
// aparelho que some do canal sem se despedir (o PC da bancada 1) prende os buffers do Wi-Fi e a memória de todos;
// derrubá-lo libera a memória sem tirar a telemetria dos outros. Uma derrubada por vez, a cada dropGapMs, para a
// memória ter tempo de voltar.
// Sem culpado (todos progredindo, só devagar: celulares em economia de energia confirmam em lotes), ou ainda com
// pouca memória depois de dropGapMs, a telemetria pausa para todos (como antes) até o heap passar de resumeKb.
// Durante a pausa, um culpado que apareça também é derrubado.
#pragma once
#include <stdint.h>

// Progresso no envio pelas confirmações: os bytes sem confirmação zeraram ou diminuíram desde a consulta anterior
// (-1 = sem socket). A mesma regra da pausa e da escolha do culpado: sem ela, um cliente recebendo confirmações, mas sem
// escrita nova (acima de WS_MAX_INFLIGHT), parecia parado e podia ser derrubado (revisão de 27/10/2026)
inline bool heapProgressed(int before, int now) { return now == 0 || (now > 0 && before >= 0 && now < before); }

struct HeapClient {
    bool     ready;      // conexão aberta
    bool     pending;    // quadro por sair ou bytes sem confirmação no TCP
    uint32_t lastOkMs;   // último progresso no envio (millis)
};

struct HeapGuardConfig {
    uint32_t pauseKb, resumeKb;   // pausa abaixo de pauseKb; retoma a partir de resumeKb
    uint32_t stallMs;             // sem progresso há pelo menos isso, com algo pendente: culpado
    uint32_t dropGapMs;           // intervalo mínimo entre duas derrubadas (e antes de pausar depois de uma)
};

enum class HeapAction : uint8_t { None, Drop, Pause, Resume };

struct HeapDecision {
    HeapAction action;
    int        client;      // com Drop: o cliente a derrubar
    uint32_t   stalledMs;   // com Drop: há quanto tempo ele estava sem progresso
};

class HeapGuard {
public:
    explicit HeapGuard(const HeapGuardConfig &cfg) : cfg_(cfg) {}
    // Uma decisão por volta da task. clients pode ser nulo com n = 0 quando o heap está acima de pauseKb (só a
    // retomada é decidida)
    HeapDecision step(uint32_t freeKb, uint32_t nowMs, const HeapClient *clients, int n);
    bool paused() const { return paused_; }

private:
    int culprit(uint32_t nowMs, const HeapClient *clients, int n, uint32_t &stalledMs) const;

    HeapGuardConfig cfg_;
    bool paused_ = false;
    bool dropped_ = false;
    uint32_t lastDropMs_ = 0;
};
