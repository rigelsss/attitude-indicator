// encoder_decode.h — decodificação do encoder KY-040 e do botão dele (docs/solo.md, passo 2).
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Giro: o estado é (CLK << 1) | DT. Cada transição válida da quadratura soma ±1; um clique é emitido quando a
// soma chega a ±stepsPerClick num estado de repouso. O KY-040 da bancada tem 2 passos por clique e repousa em
// 11 e em 00 (hardware.md, 23/10/2026); com 4 passos por clique, o repouso é só 11. Um repique (ida e volta)
// se cancela na soma; ao chegar num repouso sem completar o clique, a soma recomeça.
// Uma transição que pula um estado é contada como inválida e zera a soma: o clique parcial não se junta a passos
// do outro lado da transição ambígua, e a contagem só recomeça no próximo repouso. O mesmo vale para uma
// lacuna na sequência (estados descartados com a fila da ISR cheia, resync()): sem isso, dois estados perdidos
// num giro horário (11 → 01 → 00 → 10 lido como 11 → 10) virariam um passo anti-horário.
// Botão: consulta periódica com debounce. Curto = solto antes de longMs; longo = avisado ao completar longMs,
// ainda pressionado, e sem curto ao soltar.
#pragma once
#include <stdint.h>

struct EncoderConfig {
    int8_t stepsPerClick;   // 2 ou 4
    int8_t dir;             // +1: horário positivo; −1 inverte
};

class EncoderDecoder {
public:
    EncoderDecoder(const EncoderConfig &cfg, uint8_t initialState);
    // Próximo estado lido (2 bits). Devolve +1 ou −1 num clique completo, 0 fora disso
    int8_t feed(uint8_t state);
    // Lacuna na sequência: descarta o clique parcial; o próximo estado só é registrado, e a contagem recomeça no
    // próximo repouso
    void resync();
    uint32_t invalid() const { return invalid_; }

private:
    bool isDetent(uint8_t state) const;

    EncoderConfig cfg_;
    uint8_t       prev_;
    int8_t        acc_ = 0;
    bool          synced_ = true;      // false depois de uma inválida ou lacuna, até o próximo repouso
    bool          havePrev_ = true;    // false depois de uma lacuna: o próximo estado não forma transição
    uint32_t      invalid_ = 0;
};

enum class ButtonEvent : uint8_t { None, Short, Long };

class ButtonDebounce {
public:
    ButtonDebounce(uint32_t debounceMs, uint32_t longMs) : debounceMs_(debounceMs), longMs_(longMs) {}
    // Leitura crua (true = pressionado) no instante nowMs
    ButtonEvent update(bool pressedRaw, uint32_t nowMs);
    bool pressed() const { return stable_; }

private:
    uint32_t debounceMs_, longMs_;
    bool     stable_ = false, lastRaw_ = false, longSent_ = false;
    uint32_t changeMs_ = 0, downMs_ = 0;
};
