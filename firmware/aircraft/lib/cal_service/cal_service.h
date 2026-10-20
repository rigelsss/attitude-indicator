// cal_service.h — serviço da calibração do MPU6050: carga da NVS no boot (acelerômetro, bias do
// giroscópio e montagem), calibração do giroscópio (boot e g, lib/cal_flow), gravações, mensagens
// [cal] e aviso de temperatura. Imprime pelo lib/logger. Docs: docs/calibracao.md e docs/refatoracao.md.
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

    // Uma amostra válida, fora do descarte de partida. Devolve Six ou Level quando a janela dessas
    // calibrações termina (tratadas no main.cpp até o passo 6b); o giroscópio é tratado aqui.
    CalCoordinator::Done sample(const ImuSample &s);
    void holdWindowStart(uint32_t tUs) { coord_.holdWindowStart(tUs); }
    void onMpuReconfigured();
    // Aviso único quando o chip se afasta da temperatura da calibração do giroscópio
    void checkTempDrift(float nowC);

    // Seis posições e montagem (fluxos no main.cpp até o passo 6b)
    void endSix();                                    // retoma a calibração do boot se ela ainda falta
    bool saveAccel(const AccelCal &cal, float tempC);  // grava e passa a usar
    bool saveLevel(const LevelData &d);               // idem
    void clearLevel() { level_ = LevelData(); }
    CalCoordinator &coord() { return coord_; }        // provisório até o passo 6b

    // Estado, para a fusão, o console e os bloqueios
    const ImuCalData &imuCal() const { return cal_; }
    const LevelData &level() const { return level_; }
    const Vec3 &gyroBias() const { return coord_.gyro().bias(); }
    float gyroBiasTempC() const { return coord_.gyro().biasTempC(); }
    bool gyroDefinitive() const { return coord_.gyro().definitive(); }
    CalMode mode() const { return coord_.mode(); }
    bool magAllowed() const { return coord_.magAllowed(); }
    MountStart checkMount() const { return coord_.checkMount(cal_.accelValid); }

private:
    void printGyroStart() const;
    void onGyroEvent();

    CalServiceConfig cfg_;
    CalCoordinator coord_;
    ImuCalData cal_;      // o que está na NVS (e em uso para o acelerômetro)
    LevelData level_;     // rotação de montagem em uso (identidade sem calibração)
    bool tempWarned_ = false;
};
