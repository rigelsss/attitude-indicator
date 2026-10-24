// cal_service.h — serviço da calibração do MPU6050: carga da NVS no boot (acelerômetro, bias do
// giroscópio e montagem), calibrações do giroscópio (boot e g), das seis posições (6) e da montagem (z)
// (lib/cal_flow), gravações, mensagens [cal] e [mont] e aviso de temperatura. Imprime pelo lib/logger.
// Docs: docs/calibracao.md, docs/fusao.md e docs/refatoracao.md.
// Recebe as amostras do laço de aquisição: deve ser chamado só pela task dona dele (hoje, o loop()).
#pragma once
#include <Arduino.h>
#include "cal_flow.h"
#include "imu_store.h"
#include "level_store.h"
#include "mpu6050.h"

// Valores do config.h, montados por quem cria o serviço (as libs não incluem o config.h)
struct CalServiceConfig {
    CalCoordinatorParams flow;
    uint8_t accelRangeG;           // faixa do acelerômetro em uso (registro da NVS por faixa)
    float   storeAccOffsetMaxG;    // NVS: registro corrompido acima disto
    float   storeGyroBiasMaxDps;
    float   levelMaxTiltDeg;       // NVS: montagem aceita até este ângulo
    float   tempWarnC;             // aviso: chip a mais disto da temperatura da calibração do giroscópio
};

// Resultado de uma tecla entregue ao serviço
enum class CalKey : uint8_t {
    NotHandled,           // fora das seis posições e da montagem: a tecla segue para os outros comandos
    Handled,
    HandledFusionReset,   // calibração gravada e em uso: a referência da fusão mudou (reiniciar a fusão)
};

class CalService {
public:
    explicit CalService(const CalServiceConfig &cfg) : cfg_(cfg), coord_(cfg.flow) {}

    // Boot: carrega a calibração e imprime os avisos de registros ignorados. O bias salvo fica em uso,
    // provisório, até a calibração deste boot ser aceita.
    void loadFromNvs();
    // Linha-resumo da carga (depois da carga do magnetômetro, que imprime os avisos dele)
    void printNvsSummary(bool magCalibrated) const;

    // Comandos
    void startGyro(GyroCalKind kind);   // boot e tecla g
    void startSix();                    // tecla 6
    void startMount();                  // tecla z (com os pré-requisitos)
    // Teclas durante as seis posições e a montagem (espera: Enter e x; medida: x; revisão: s e x).
    // Nesses modos, as outras teclas são ignoradas.
    CalKey handleKey(char ch);

    // Uma amostra válida, fora do descarte de partida
    void sample(const ImuSample &s);
    void holdWindowStart(uint32_t tUs) { coord_.holdWindowStart(tUs); }
    void onMpuReconfigured();
    // Aviso único quando o chip se afasta da temperatura da calibração do giroscópio
    void checkTempDrift(float nowC);

    // Estado, para a fusão, o console e os bloqueios
    const ImuCalData &imuCal() const { return cal_; }
    const LevelData &level() const { return level_; }
    const Vec3 &gyroBias() const { return coord_.gyro().bias(); }
    float gyroBiasTempC() const { return coord_.gyro().biasTempC(); }
    bool gyroDefinitive() const { return coord_.gyro().definitive(); }
    CalMode mode() const { return coord_.mode(); }
    bool magAllowed() const { return coord_.magAllowed(); }
    // Fim das calibrações do giroscópio pedidas por comando (g ou rádio): o contador sobe a cada uma, aceita ou
    // recusada, e o último resultado fica em gyroCmdAccepted() (comandos do rádio, docs/web.md)
    uint32_t gyroCmdOutcomes() const { return gyroCmdOutcomes_; }
    bool gyroCmdAccepted() const { return gyroCmdAccepted_; }

private:
    void printGyroStart() const;
    void promptSixPose() const;
    void promptMount() const;
    void onGyroEvent();
    void onSixEvent();
    void onMountEvent();
    // Sai das seis posições ou da montagem e, se a calibração do boot recomeçou, avisa e abre a janela
    void exitFlow();
    bool saveSix();
    bool saveMount();

    CalServiceConfig cfg_;
    CalCoordinator coord_;
    ImuCalData cal_;      // o que está na NVS (e em uso para o acelerômetro)
    LevelData level_;     // rotação de montagem em uso (identidade sem calibração)
    bool tempWarned_ = false;
    uint32_t gyroCmdOutcomes_ = 0;
    bool gyroCmdAccepted_ = false;
};
