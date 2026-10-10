// mpu6050.h — driver do MPU6050 por acesso direto aos registradores (I2C).
// Entrega aceleração em g, velocidade angular em °/s e temperatura em °C, nos eixos do chip.
// Não conhece calibração, eixos do modelo, rede nem display: quem o usa passa a configuração
// (vinda do config.h) e aplica calibração e mapeamento de eixos depois.
// Não é thread-safe: deve ser usado por uma única task.
#pragma once
#include <Arduino.h>
#include <Wire.h>

struct Mpu6050Config {
    TwoWire *wire;            // barramento já iniciado por quem usa o driver
    int      pinSda;          // pinos e frequência: usados só para recuperar o barramento
    int      pinScl;
    uint32_t i2cFreqHz;
    uint8_t  addr;            // 0x68 com AD0 em nível baixo
    uint8_t  accelRangeG;     // 2, 4, 8 ou 16
    uint16_t gyroRangeDps;    // 250, 500, 1000 ou 2000
    uint8_t  dlpfHz;          // banda do giroscópio: 188, 98, 42, 20, 10 ou 5
    uint16_t sampleRateHz;    // 4 a 1000 (base de 1 kHz com o DLPF ligado)
    uint8_t  maxFailStreak;   // falhas seguidas antes de reiniciar o barramento
};

enum class Mpu6050Status : uint8_t {
    Ok,
    BadParam,     // valor de configuração não suportado
    NoAck,        // ninguém respondeu no endereço
    WrongId,      // respondeu, mas WHO_AM_I diferente de 0x68
    ConfigFail,   // escrita de registrador não confirmada na releitura
};

const char *mpu6050StatusText(Mpu6050Status s);

struct ImuSample {
    float    ax, ay, az;   // g
    float    gx, gy, gz;   // °/s
    float    tempC;        // °C
    uint32_t tUs;          // micros() no início da leitura
    bool     saturated;    // algum eixo atingiu o limite da faixa
};

class Mpu6050 {
public:
    // Configura o chip (reset, relógio, faixas, DLPF, taxa e INT de "data ready").
    Mpu6050Status begin(const Mpu6050Config &cfg);

    // Lê uma amostra. Em caso de falha devolve false e não altera `out`.
    // Depois de maxFailStreak falhas seguidas, tenta reiniciar o barramento e o chip.
    // Quando a comunicação volta, confere a configuração antes de aceitar dados: se o chip
    // reiniciou (queda de alimentação), ele volta em sleep e devolveria valores parados.
    bool read(ImuSample &out);

    // Para quando os pulsos de "data ready" param: sem eles ninguém chama read() e uma falha
    // passaria despercebida. Confere se o chip responde e mantém a configuração; sem resposta
    // conta como falha (mesmo fluxo de recuperação), configuração perdida → reconfigura.
    // Devolve true se o chip responde com a configuração certa.
    bool checkAlive();

    bool     ok() const { return configured_ && failStreak_ < cfg_.maxFailStreak; }
    uint8_t  whoAmI() const { return whoAmI_; }
    float    actualSampleRateHz() const { return actualRateHz_; }
    uint32_t failTotal() const { return failTotal_; }
    uint32_t failStreak() const { return failStreak_; }
    uint32_t recoveries() const { return recoveries_; }

private:
    enum class Check : uint8_t { Ok, Mismatch, BusFail };

    Mpu6050Status configure();
    Check verifyConfig();
    void handleFail();
    void recoverBus();
    bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n);
    bool writeReg(uint8_t reg, uint8_t val);
    bool writeVerify(uint8_t reg, uint8_t val);

    Mpu6050Config cfg_{};
    bool     configured_ = false;
    bool     needsVerify_ = false;   // houve uma sequência de falhas: conferir a configuração
    uint8_t  whoAmI_ = 0;
    uint8_t  dlpfCfg_ = 0, gyroFsSel_ = 0, accelFsSel_ = 0, smplrtDiv_ = 0;
    float    accelLsbPerG_ = 1.0f;
    float    gyroLsbPerDps_ = 1.0f;
    float    actualRateHz_ = 0.0f;
    uint32_t failTotal_ = 0;
    uint32_t failStreak_ = 0;
    uint32_t recoveries_ = 0;
};
