// mag_service.h — serviço do magnetômetro (HMC5883L): configuração com nova tentativa, leitura
// periódica, recuperação, correção da proa pelo magnetômetro, aviso de distúrbio, calibração m
// (MagCalibrator, lib/mag; gravação em lib/mag_store), fluxo p e resumo. Imprime pelo lib/logger.
// Docs: docs/magnetometro.md e docs/refatoracao.md.
// Usa o barramento de sensores: deve ser chamado só pela task dona dele (hoje, o loop()).
#pragma once
#include <Arduino.h>
#include "hmc5883l.h"
#include "mag.h"
#include "mag_store.h"
#include "attitude.h"
#include "sched.h"

// Resumo de uma linha (r e c), copiado na task dona do barramento e formatado na task da serial (por cópia:
// docs/tasks.md). Os textos apontam para literais, válidos em qualquer task
struct MagSummary {
    bool        ready;         // begin() aceito
    const char *statusText;    // sem ready: motivo da última tentativa
    bool        ok;
    uint32_t    winReads;      // leituras válidas no último 1 s
    bool        have;          // há uma leitura válida
    float       normUt;        // |m| corrigido
    bool        hdgInitialized;
    float       hdgDeg;
    const char *state;         // stateText()
    bool        calValid;
    uint32_t    failTotal, failStreak, recoveries, saturations;
};

// Valores do config.h, montados por quem cria o serviço (as libs não incluem o config.h)
struct MagServiceConfig {
    Hmc5883lConfig chip;
    MagCalParams   cal;      // maxPoints: capacidade do buffer, alocado só durante a calibração
    uint32_t readHz;         // leituras por segundo com o chip respondendo
    uint32_t retryMs;        // chip ausente no boot: nova tentativa de configuração
    uint32_t failRetryMs;    // chip falhando (ok() falso): uma leitura por intervalo
    float    disturbPct;     // |m| afastado do raio da calibração mais que isto: começa o distúrbio
    float    disturbExitPct; // e volta a no máximo isto: termina (histerese, menor que disturbPct)
    uint32_t disturbExitMs;  // por este tempo seguido, para terminar
    float    refFieldUt;     // |B| do modelo geomagnético na bancada (só informativo, no fim da calibração)
    uint32_t printHz;        // fluxo p
    uint32_t keyHintMs;      // calibração: intervalo mínimo entre os avisos de tecla ignorada
};

// Revisão decidida por comando do rádio (decideReview)
enum class MagReview : uint8_t {
    None,         // fora da revisão
    Saved,        // ajuste aceito pelos critérios e gravado
    SaveFailed,   // aceito, mas a gravação na NVS falhou
    Rejected,     // recusado pelos critérios: descartado
};

// Resultado de uma tecla entregue ao serviço
enum class MagKey : uint8_t {
    NotHandled,         // fora da calibração: a tecla segue para os outros comandos
    Handled,
    HandledLoopStalled, // d: a impressão dos pontos parou o laço por alguns segundos (reiniciar a fusão)
};

class MagService {
public:
    // att: atitude para a compensação de inclinação; hdg: filtro da proa, corrigido aqui e
    // reiniciado ao começar e ao gravar uma calibração
    MagService(const MagServiceConfig &cfg, const AttitudeFilter &att, HeadingFilter &hdg);
    // Carrega a calibração da NVS (no boot) e avisa se o registro foi ignorado
    void loadCalibration();
    // Primeira tentativa de configuração (no boot, com o Wire já iniciado)
    void begin();
    // Chamada logo depois de cada leitura do MPU, como o barômetro: a rajada de 9 bytes (~0,25 ms a
    // 400 kHz) fica longe do próximo pulso do INT
    void service(uint32_t nowUs);

    // Comandos
    // m. otherCalRunning: outra calibração em andamento (a do giroscópio no boot não bloqueia).
    // Recusado também com uma calibração do magnetômetro já em andamento.
    void startCalibration(bool otherCalRunning);
    void toggleStream(uint32_t nowUs);   // p
    // Teclas durante a calibração (coleta: só x; revisão: s, x, d)
    MagKey handleKey(char ch);
    // Calibração pedida pelo rádio: na revisão, grava sozinha se o ajuste passou nos critérios de aceitação (os
    // mesmos do s), senão descarta (docs/web.md). Fora da revisão, None
    MagReview decideReview();
    bool reviewing() const { return calib_.state() == MagCalibrator::State::Review; }

    // Fecha a janela de 1 s do resumo (contagem de leituras)
    void closeWindow();
    // Uma linha no resumo por segundo (r) e no estado (c)
    MagSummary summary() const;   // uma linha no resumo por segundo (r) e no estado (c), por cópia
    const char *stateText() const;

    // Estado, para o console, a fusão e a validade da telemetria
    bool calibrating() const { return calib_.state() != MagCalibrator::State::Off; }
    bool ok() const { return ready_ && mag_.ok(); }   // configurado e respondendo (ST_MAG_OK)
    const MagCalData &calData() const { return cal_; }
    // A correção da proa pelo magnetômetro está suspensa (inclinação, distúrbio, atitude degradada, dado inválido)
    bool correctionSuspended() const;
    bool lastReadValid() const { return prevValid_; }   // há uma leitura de referência (zera ao sair da calibração)
    uint32_t lastReadMs() const { return prevMs_; }

private:
    void start();
    void freeCalibration();
    bool saveFit();
    void printFinish();
    void dumpPoints();
    void printLine() const;
    void hintIgnoredKey(char ch, const char *msg);

    MagServiceConfig cfg_;
    const AttitudeFilter &att_;
    HeadingFilter &hdg_;
    uint32_t periodUs_;
    uint32_t printUs_;
    Hmc5883l mag_;
    bool ready_ = false;   // begin() aceito; depois disso o driver se recupera sozinho
    Hmc5883lStatus lastStatus_ = Hmc5883lStatus::Ok;   // evita repetir a mesma mensagem a cada tentativa
    uint32_t retryAtMs_ = 0;
    Periodic sched_;       // próxima leitura
    uint32_t lastRecoveries_ = 0;
    MagCalData cal_;       // calibração em uso (offset zero e W identidade sem calibração)
    bool have_ = false;    // há uma leitura válida
    Vec3 corr_{0, 0, 0};   // última leitura, eixos do modelo, calibrada (µT)
    bool prevValid_ = false;   // há uma leitura anterior (referência do dt da correção)
    uint32_t prevMs_ = 0;
    bool haveTilt_ = false;    // proa compensada calculada (atitude disponível)
    MagTilt tilt_ = {0, 0};
    HeadingFilter::Mag step_ = HeadingFilter::Mag::Corrected;   // resultado da última correção
    MagDisturbance disturb_;   // distúrbio com histerese: suspende a correção e gera o aviso
    uint32_t winReads_ = 0;    // leituras válidas na janela do resumo
    uint32_t lastWinReads_ = 0;
    MagCalibrator calib_;
    Vec3 *pts_ = nullptr;      // buffer da calibração (cal.maxPoints × 12 B, ~54 kB)
    bool streamOn_ = false;
    Periodic streamPrint_;
    // Último aviso de tecla ignorada (millis, diferença sem sinal: vale com a revisão aberta por horas)
    bool keyHinted_ = false;
    uint32_t keyHintMs_ = 0;
};
