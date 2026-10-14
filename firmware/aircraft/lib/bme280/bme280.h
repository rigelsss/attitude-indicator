// bme280.h — driver do BME280 ou do BMP280 (temperatura e pressão) por acesso direto aos registradores (I2C).
// Os dois chips têm os mesmos registradores de temperatura e pressão, coeficientes, reset e status; o
// BMP280 (chip_id 0x58) não tem umidade nem o registrador ctrl_hum (0xF2).
// Modo normal: o chip mede sozinho e o driver só lê o último resultado, sem esperar a conversão.
// Entrega temperatura em °C e pressão em hPa; a compensação está em lib/baro (lógica pura).
// Não conhece altitude, rede nem display. A umidade do BME280 fica desligada.
// Não reinicia o barramento: isso é feito pelo driver do MPU6050, dono do barramento compartilhado.
// Não é thread-safe: deve ser usado por uma única task.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "baro.h"

struct Bme280Config {
    TwoWire *wire;            // barramento já iniciado por quem usa o driver
    uint8_t  addr;            // 0x76 com SDO em GND
    uint8_t  osrsT;           // oversampling da temperatura: 1, 2, 4, 8 ou 16
    uint8_t  osrsP;           // oversampling da pressão: 1, 2, 4, 8 ou 16
    uint8_t  iirCoef;         // filtro IIR: 0 (desligado), 2, 4, 8 ou 16
    uint32_t standbyUs;       // espera entre medições no modo normal: 500, 62500, 125000, 250000, 500000
                              // ou 1000000 nos dois chips; mais 10000 e 20000 só no BME280, e 2000000 e
                              // 4000000 só no BMP280 (conferido em configure(), pelo chip_id: BadParam)
    uint8_t  maxFailStreak;   // falhas seguidas até ok() ficar falso
};

enum class Bme280Status : uint8_t {
    Ok,
    BadParam,       // valor de configuração não suportado
    NoAck,          // ninguém respondeu no endereço
    WrongId,        // respondeu, mas chip_id diferente de 0x60 (BME280) e de 0x58 (BMP280)
    ResetTimeout,   // a cópia dos coeficientes após o reset não terminou
    BadCalib,       // coeficientes de fábrica inválidos
    ConfigFail,     // escrita de registrador não confirmada na releitura
};

const char *bme280StatusText(Bme280Status s);

struct BaroSample {
    float    tempC;      // °C (temperatura do chip)
    float    pressHpa;   // hPa
    uint32_t tMs;        // millis() da leitura
};

class Bme280 {
public:
    // Soft reset, leitura dos coeficientes e configuração (oversampling, filtro, modo normal).
    Bme280Status begin(const Bme280Config &cfg);

    // Lê o último resultado. Devolve false em falha de comunicação, sem medição pronta (logo após
    // configurar) ou quando o chip perdeu a configuração; nesse caso ele é reconfigurado aqui
    // (queda de alimentação: o BME280 volta em sleep e os dados parariam de mudar). Uma
    // configuração que falhou é refeita na chamada seguinte. Conta como falha: transação sem
    // resposta, reconfiguração que falhou, compensação inválida e "não medido" depois da
    // tolerância inicial. A sequência de falhas só termina com uma amostra válida.
    bool read(BaroSample &out);

    bool     ok() const { return configured_ && failStreak_ < cfg_.maxFailStreak; }
    bool     configured() const { return configured_; }   // última configuração confirmada na releitura
    uint8_t  chipId() const { return chipId_; }
    const char *model() const;   // "BME280" ou "BMP280", pelo último chip_id lido
    uint32_t failTotal() const { return failTotal_; }
    uint32_t failStreak() const { return failStreak_; }
    uint32_t recoveries() const { return recoveries_; }

private:
    Bme280Status configure();
    Bme280Status reconfigure();   // configure() + estado (configured_, instante da configuração)
    void handleFail();
    bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n);
    bool writeReg(uint8_t reg, uint8_t val);

    Bme280Config cfg_{};
    Bme280Calib  calib_{};
    bool     configured_ = false;
    uint8_t  chipId_ = 0;
    uint8_t  ctrlMeas_ = 0, config_ = 0;   // config_ depende do modelo (t_sb): montado em configure()
    uint8_t  iirCode_ = 0;
    uint32_t configuredAtMs_ = 0;
    uint32_t skipGraceMs_ = 0;    // tolerância ao "não medido" após configurar
    uint32_t failTotal_ = 0;
    uint32_t failStreak_ = 0;
    uint32_t recoveries_ = 0;
};
