// hmc5883l.h — driver do HMC5883L (GY-273) por acesso direto aos registradores (I2C).
// Modo contínuo: o chip mede sozinho e o driver só lê o último resultado.
// Entrega o campo em µT nos eixos do chip; decodificação e conversões estão em lib/mag (lógica pura).
// Não conhece proa, rede nem display.
// Não reinicia o barramento: isso é feito pelo driver do MPU6050, dono do barramento compartilhado.
// Não é thread-safe: deve ser usado por uma única task.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "mag.h"

struct Hmc5883lConfig {
    TwoWire *wire;            // barramento já iniciado por quem usa o driver
    uint8_t  addr;            // 0x1E
    uint8_t  avgSamples;      // média no chip: 1, 2, 4 ou 8
    float    outputHz;        // taxa de saída: 0,75, 1,5, 3, 7,5, 15, 30 ou 75 Hz
    uint16_t rangeMga;        // faixa: 880, 1300, 1900, 2500, 4000, 4700, 5600 ou 8100 mGa
    uint8_t  maxFailStreak;   // falhas seguidas até ok() ficar falso
};

enum class Hmc5883lStatus : uint8_t {
    Ok,
    BadParam,     // valor de configuração não suportado
    NoAck,        // ninguém respondeu no endereço
    WrongId,      // respondeu, mas o ID não é "H43" (um clone QMC5883L fica em 0x0D)
    ConfigFail,   // escrita de registrador não confirmada na releitura
};

const char *hmc5883lStatusText(Hmc5883lStatus s);

struct MagSample {
    Vec3     chipUt;   // µT, eixos do chip
    uint32_t tMs;      // millis() da leitura
};

class Hmc5883l {
public:
    // Confere o ID e configura média, taxa, ganho e modo contínuo.
    Hmc5883lStatus begin(const Hmc5883lConfig &cfg);

    // Lê a configuração e os dados numa rajada de 9 bytes a partir de 0x00. Devolve false em falha de
    // comunicação, saturação, logo após configurar (primeira medição ainda não pronta) ou quando o chip
    // perdeu a configuração; nesse caso ele é reconfigurado aqui (queda de alimentação: o HMC volta em
    // medição única e os dados parariam de mudar). Conta como falha: transação sem resposta e
    // reconfiguração que falhou. A sequência de falhas só termina com uma amostra válida.
    bool read(MagSample &out);

    bool     ok() const { return configured_ && failStreak_ < cfg_.maxFailStreak; }
    bool     configured() const { return configured_; }
    uint8_t  crb() const { return crb_; }
    float    lsbPerGa() const { return lsbPerGa_; }
    uint32_t failTotal() const { return failTotal_; }
    uint32_t failStreak() const { return failStreak_; }
    uint32_t recoveries() const { return recoveries_; }
    uint32_t saturations() const { return saturations_; }

private:
    Hmc5883lStatus configure();
    Hmc5883lStatus reconfigure();   // configure() + estado (configured_, instante da configuração)
    void handleFail();
    bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n);
    bool writeReg(uint8_t reg, uint8_t val);

    Hmc5883lConfig cfg_{};
    uint8_t  cra_ = 0, crb_ = 0;
    float    lsbPerGa_ = 0;
    bool     configured_ = false;
    uint32_t configuredAtMs_ = 0;
    uint32_t settleMs_ = 0;     // descarte após configurar: a primeira saída ainda não foi medida
    uint32_t failTotal_ = 0;
    uint32_t failStreak_ = 0;
    uint32_t recoveries_ = 0;
    uint32_t saturations_ = 0;
};
