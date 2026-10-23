// send_gate.h — um envio ESP-NOW em voo por vez (docs/solo.md, passo 4; docs/radio.md). Lógica pura (sem
// Arduino.h), testável com pio test -e native.
//
// O callback de envio do ESP-NOW não identifica o pacote que confirma. Por isso o envio em voo nunca é liberado
// só pelo tempo: um callback atrasado do envio antigo liberaria o novo, e dois ficariam em voo. Regras:
//   - sem envio em voo: envia (Send);
//   - em voo há menos de stuckMs: pula (Skip). Ao passar de timeoutMs, o episódio é contado uma vez (timeouts);
//   - em voo há stuckMs ou mais: recuperação (Recover). Quem chama reinicia o ESP-NOW (deinit + init), o que
//     descarta o envio pendente e qualquer callback dele, e então chama recovered().
// O callback é aceito só com um envio em voo; fora disso, é contado como inesperado e ignorado.
#pragma once
#include <stdint.h>

enum class GateAction : uint8_t { Send, Skip, Recover };

class SendGate {
public:
    SendGate(uint32_t timeoutMs, uint32_t stuckMs) : timeoutMs_(timeoutMs), stuckMs_(stuckMs) {}
    // Decide o ciclo no instante nowMs
    GateAction poll(uint32_t nowMs);
    // esp_now_send aceito: o envio entra em voo (chamar antes do envio: o callback pode vir antes do retorno)
    void sent(uint32_t nowMs);
    // esp_now_send recusado: não haverá callback
    void sendFailed();
    // Callback de envio (entregue ou falha MAC)
    void callback();
    // ESP-NOW reiniciado depois de Recover
    void recovered();

    bool     inFlight() const { return inFlight_; }
    uint32_t timeouts() const { return timeouts_; }
    uint32_t recoveries() const { return recoveries_; }
    uint32_t unexpectedCallbacks() const { return unexpected_; }

private:
    uint32_t timeoutMs_, stuckMs_;
    bool     inFlight_ = false, timeoutCounted_ = false;
    uint32_t sinceMs_ = 0;
    uint32_t timeouts_ = 0, recoveries_ = 0, unexpected_ = 0;
};
