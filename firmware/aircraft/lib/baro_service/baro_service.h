// baro_service.h — serviço do barômetro (BME280/BMP280): configuração com nova tentativa, leitura
// periódica, recuperação, zero da altitude (BaroZero, lib/baro), aviso de temperatura, fluxo b e
// resumo. Imprime pelo lib/logger. Docs: docs/barometro.md e docs/refatoracao.md.
// Usa o barramento de sensores: deve ser chamado só pela task dona dele (hoje, o loop()).
#pragma once
#include <Arduino.h>
#include "bme280.h"
#include "baro.h"
#include "sched.h"

// Resumo de uma linha (r e c), copiado na task dona do barramento e formatado na task da serial (por cópia:
// docs/tasks.md). Os textos apontam para literais, válidos em qualquer task
struct BaroSummary {
    bool        ready;         // begin() aceito
    const char *statusText;    // sem ready: motivo da última tentativa
    bool        ok;            // respondendo (sem ST_BARO_OK caído)
    bool        have;          // há uma leitura válida
    float       pressHpa;
    float       tempC;
    float       altM;          // NAN sem zero ou fora da faixa
    bool        zeroValid;
    float       zeroTempC;
    uint32_t    failTotal, failStreak, recoveries;
};

// Valores do config.h, montados por quem cria o serviço (as libs não incluem o config.h)
struct BaroServiceConfig {
    Bme280Config   chip;
    BaroZeroParams zero;
    uint32_t readHz;        // leituras por segundo com o chip respondendo
    uint32_t retryMs;       // chip ausente no boot: nova tentativa de configuração
    uint32_t failRetryMs;   // chip falhando (ok() falso): uma leitura por intervalo
    float    tempWarnC;     // aviso: chip a mais disto da temperatura do zero
    uint32_t printHz;       // fluxo b
};

class BaroService {
public:
    explicit BaroService(const BaroServiceConfig &cfg);
    // Primeira tentativa de configuração (no boot, com o Wire já iniciado)
    void begin();
    // Chamada logo depois de cada leitura do MPU: a transação (~0,3 ms) fica longe do próximo pulso
    // do INT, cujas bordas induzem ruído no SDA/SCL. Sem o chip, tenta configurá-lo a cada retryMs.
    void service(uint32_t nowUs);

    // Comandos
    void zeroAltitude();                 // tecla 0
    void toggleStream(uint32_t nowUs);   // tecla b

    // Uma linha no resumo por segundo (r) e no estado (c), por cópia
    BaroSummary summary() const;

    // Estado, para o console e a validade da telemetria
    bool ok() const { return ready_ && baro_.ok(); }
    bool have() const { return have_; }
    const BaroSample &last() const { return last_; }
    bool zeroValid() const { return zero_.refValid(); }
    bool zeroActive() const { return zero_.active(); }
    float zeroRefHpa() const { return zero_.refHpa(); }
    float zeroRefTempC() const { return zero_.refTempC(); }
    // Altitude da última leitura em relação ao zero; NAN sem leitura, sem zero ou fora da faixa
    float altitudeM() const;

private:
    void start();
    void startZero(bool settle);
    void onZeroEvent(BaroZero::Event ev);
    void printLine(const BaroSample &b) const;

    BaroServiceConfig cfg_;
    uint32_t periodUs_;
    uint32_t printUs_;
    Bme280 baro_;
    bool ready_ = false;   // begin() aceito; depois disso o driver se recupera sozinho
    Bme280Status lastStatus_ = Bme280Status::Ok;   // evita repetir a mesma mensagem a cada tentativa
    uint32_t retryAtMs_ = 0;
    Periodic sched_;       // próxima leitura
    uint32_t lastRecoveries_ = 0;
    BaroSample last_ = {0, 0, 0};   // última leitura válida
    bool have_ = false;
    BaroZero zero_;
    bool tempWarned_ = false;
    bool streamOn_ = false;
    Periodic streamPrint_;
};
