// ws2812_core.h — sequência de envio e recuperação do driver da fita WS2812 (docs/caixa-b.md, "Driver próprio do
// RMT"), sem o ESP-IDF: as operações do RMT chegam por Ws2812Ops. No ESP32, lib/ws2812_rmt passa as funções
// reais; no PC, os testes passam funções simuladas (prazo vencido, falhas de desinstalação e de reinstalação).
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Envio: monta os bytes no buffer, inicia a transmissão sem espera e espera o fim com prazo. O buffer só é
// reescrito depois de a transmissão anterior terminar ou de o driver ser desinstalado (sem a interrupção e o
// tradutor, ninguém mais o lê).
//
// Recuperação (prazo vencido ou erro no envio): o driver passa a "não pronto" ANTES de começar; para o hardware,
// desinstala e instala de novo. Só volta a "pronto" se a instalação inteira der certo (configuração, driver,
// clock e tradutor, conferidos pela operação install). Se a desinstalação falhar, a interrupção ainda pode ler o
// buffer: o driver fica travado ("buffer preso") e nunca mais escreve nele, nem numa nova tentativa.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "led_ring.h"

struct Ws2812Ops {
    void *ctx;
    // Configura o canal, instala o driver, obtém o clock e registra o tradutor. true só se tudo deu certo
    bool (*install)(void *ctx);
    // Inicia a transmissão de n bytes, sem esperar o fim
    bool (*write)(void *ctx, const uint8_t *buf, size_t n);
    // Espera o fim da transmissão por até timeoutMs (0: só confere)
    bool (*waitDone)(void *ctx, uint32_t timeoutMs);
    // Para o hardware
    bool (*stop)(void *ctx);
    // Desinstala o driver (sai a interrupção e o tradutor)
    bool (*uninstall)(void *ctx);
};

enum class Ws2812State : uint8_t {
    Off,           // begin() não foi chamado ou a primeira instalação falhou
    Ready,
    NotInstalled,  // a recuperação desinstalou, mas a reinstalação falhou: retry() pode tentar de novo
    BufferLocked,  // a desinstalação falhou: o buffer pode estar em uso; nenhum envio nem nova tentativa
};
const char *ws2812StateText(Ws2812State s);

class Ws2812Driver {
public:
    // buf: count·3 bytes, do chamador (reservado uma vez). grb: ordem das cores da fita
    bool begin(const Ws2812Ops &ops, uint8_t *buf, uint16_t count, bool grb);
    // Envia o quadro. false: não pronto (volta na hora, sem tocar no buffer) ou prazo vencido / erro no envio
    // (nesse caso a recuperação já rodou antes de voltar)
    bool show(const LedRgb *colors, uint16_t count, uint32_t timeoutMs);
    // Nova instalação depois de uma recuperação malsucedida (NotInstalled). false nos outros estados ou se falhar
    bool retry();
    bool ready() const { return state_ == Ws2812State::Ready; }
    Ws2812State state() const { return state_; }
    uint32_t failures() const { return failures_; }       // envios com prazo vencido ou erro
    uint32_t recoveries() const { return recoveries_; }   // recuperações executadas
    uint32_t recoveryFailures() const { return recoveryFailures_; }

private:
    void recover();
    Ws2812Ops   ops_ = {};
    uint8_t    *buf_ = nullptr;
    uint16_t    count_ = 0;
    bool        grb_ = true;
    Ws2812State state_ = Ws2812State::Off;
    uint32_t    failures_ = 0, recoveries_ = 0, recoveryFailures_ = 0;
};
