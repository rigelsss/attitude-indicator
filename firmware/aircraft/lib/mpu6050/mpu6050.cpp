// mpu6050.cpp — implementação do driver. Registradores conforme o "MPU-6000/6050 Register Map".
#include "mpu6050.h"

// Registradores
static const uint8_t REG_SMPLRT_DIV   = 0x19;   // taxa = 1 kHz / (1 + div) com o DLPF ligado
static const uint8_t REG_CONFIG       = 0x1A;   // DLPF_CFG nos bits 2..0
static const uint8_t REG_GYRO_CONFIG  = 0x1B;   // FS_SEL nos bits 4..3
static const uint8_t REG_ACCEL_CONFIG = 0x1C;   // AFS_SEL nos bits 4..3
static const uint8_t REG_INT_PIN_CFG  = 0x37;   // 0x00: ativo alto, push-pull, pulso de 50 µs
static const uint8_t REG_INT_ENABLE   = 0x38;   // bit 0: DATA_RDY_EN
static const uint8_t REG_INT_STATUS   = 0x3A;   // leitura limpa os pedidos pendentes
static const uint8_t REG_ACCEL_XOUT_H = 0x3B;   // 14 bytes: accel, temp, gyro (big-endian)
static const uint8_t REG_PWR_MGMT_1   = 0x6B;
static const uint8_t REG_WHO_AM_I     = 0x75;

static const uint8_t PWR_DEVICE_RESET = 0x80;
static const uint8_t PWR_CLKSEL_PLL_X = 0x01;   // relógio do PLL do giroscópio X, sem sleep
static const uint8_t WHO_AM_I_MPU6050 = 0x68;

const char *mpu6050StatusText(Mpu6050Status s) {
    switch (s) {
    case Mpu6050Status::Ok:         return "ok";
    case Mpu6050Status::BadParam:   return "parâmetro de configuração não suportado";
    case Mpu6050Status::NoAck:      return "sem resposta no endereço I2C";
    case Mpu6050Status::WrongId:    return "WHO_AM_I inesperado";
    case Mpu6050Status::ConfigFail: return "registrador não confirmado na releitura";
    }
    return "desconhecido";
}

// Converte os parâmetros físicos nos códigos dos registradores; false se não suportado
static bool accelCode(uint8_t rangeG, uint8_t &code, float &lsbPerG) {
    switch (rangeG) {
    case 2:  code = 0; lsbPerG = 16384.0f; return true;
    case 4:  code = 1; lsbPerG = 8192.0f;  return true;
    case 8:  code = 2; lsbPerG = 4096.0f;  return true;
    case 16: code = 3; lsbPerG = 2048.0f;  return true;
    }
    return false;
}

static bool gyroCode(uint16_t rangeDps, uint8_t &code, float &lsbPerDps) {
    switch (rangeDps) {
    case 250:  code = 0; lsbPerDps = 131.0f; return true;
    case 500:  code = 1; lsbPerDps = 65.5f;  return true;
    case 1000: code = 2; lsbPerDps = 32.8f;  return true;
    case 2000: code = 3; lsbPerDps = 16.4f;  return true;
    }
    return false;
}

// DLPF_CFG 0 (256 Hz) muda a base do giroscópio para 8 kHz; não é aceito
static bool dlpfCode(uint8_t hz, uint8_t &code) {
    switch (hz) {
    case 188: code = 1; return true;
    case 98:  code = 2; return true;
    case 42:  code = 3; return true;
    case 20:  code = 4; return true;
    case 10:  code = 5; return true;
    case 5:   code = 6; return true;
    }
    return false;
}

Mpu6050Status Mpu6050::begin(const Mpu6050Config &cfg) {
    cfg_ = cfg;
    configured_ = false;
    if (cfg_.wire == nullptr || cfg_.maxFailStreak == 0) return Mpu6050Status::BadParam;
    if (!accelCode(cfg_.accelRangeG, accelFsSel_, accelLsbPerG_)) return Mpu6050Status::BadParam;
    if (!gyroCode(cfg_.gyroRangeDps, gyroFsSel_, gyroLsbPerDps_)) return Mpu6050Status::BadParam;
    if (!dlpfCode(cfg_.dlpfHz, dlpfCfg_)) return Mpu6050Status::BadParam;
    if (cfg_.sampleRateHz < 4 || cfg_.sampleRateHz > 1000) return Mpu6050Status::BadParam;
    smplrtDiv_ = (uint8_t)(1000 / cfg_.sampleRateHz - 1);
    actualRateHz_ = 1000.0f / (1 + smplrtDiv_);

    Mpu6050Status st = configure();
    configured_ = (st == Mpu6050Status::Ok);
    return st;
}

Mpu6050Status Mpu6050::configure() {
    if (!readRegs(REG_WHO_AM_I, &whoAmI_, 1)) return Mpu6050Status::NoAck;
    if (whoAmI_ != WHO_AM_I_MPU6050) return Mpu6050Status::WrongId;

    // Reset completo: o chip volta ao padrão (±2 g, ±250 °/s, sleep) e é reconfigurado do zero
    writeReg(REG_PWR_MGMT_1, PWR_DEVICE_RESET);
    delay(100);
    if (!writeVerify(REG_PWR_MGMT_1, PWR_CLKSEL_PLL_X)) return Mpu6050Status::ConfigFail;
    delay(10);   // estabilização do PLL

    bool ok = writeVerify(REG_SMPLRT_DIV, smplrtDiv_) &&
              writeVerify(REG_CONFIG, dlpfCfg_) &&
              writeVerify(REG_GYRO_CONFIG, (uint8_t)(gyroFsSel_ << 3)) &&
              writeVerify(REG_ACCEL_CONFIG, (uint8_t)(accelFsSel_ << 3)) &&
              writeVerify(REG_INT_PIN_CFG, 0x00) &&
              writeVerify(REG_INT_ENABLE, 0x01);
    if (!ok) return Mpu6050Status::ConfigFail;

    uint8_t st;
    readRegs(REG_INT_STATUS, &st, 1);
    return Mpu6050Status::Ok;
}

// Relê os registradores de configuração e compara com o que foi gravado em configure()
Mpu6050::Check Mpu6050::verifyConfig() {
    uint8_t cfg[4];   // SMPLRT_DIV, CONFIG, GYRO_CONFIG, ACCEL_CONFIG (consecutivos)
    uint8_t pwr, inten;
    if (!readRegs(REG_SMPLRT_DIV, cfg, sizeof(cfg)) || !readRegs(REG_PWR_MGMT_1, &pwr, 1) ||
        !readRegs(REG_INT_ENABLE, &inten, 1)) {
        return Check::BusFail;
    }
    bool same = cfg[0] == smplrtDiv_ && cfg[1] == dlpfCfg_ && cfg[2] == (uint8_t)(gyroFsSel_ << 3) &&
                cfg[3] == (uint8_t)(accelFsSel_ << 3) && pwr == PWR_CLKSEL_PLL_X && inten == 0x01;
    return same ? Check::Ok : Check::Mismatch;
}

bool Mpu6050::checkAlive() {
    if (!configured_) return false;
    Check c = verifyConfig();
    if (c == Check::BusFail) {
        handleFail();
        return false;
    }
    if (c == Check::Mismatch) {
        if (configure() != Mpu6050Status::Ok) {
            handleFail();
            return false;
        }
        recoveries_++;
    }
    needsVerify_ = false;
    failStreak_ = 0;
    return true;
}

bool Mpu6050::read(ImuSample &out) {
    if (!configured_) return false;

    // Comunicação voltou depois de uma sequência de falhas: só aceita dados com a configuração
    // certa. Este ciclo é descartado; a próxima amostra já vem da configuração conferida.
    if (needsVerify_) {
        checkAlive();
        return false;
    }

    uint32_t t = micros();
    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, sizeof(b))) {
        handleFail();
        return false;
    }
    failStreak_ = 0;

    int16_t v[7];   // ax ay az temp gx gy gz
    for (int i = 0; i < 7; i++) v[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);

    bool sat = false;
    for (int i = 0; i < 7; i++) {
        if (i == 3) continue;
        if (v[i] == INT16_MAX || v[i] == INT16_MIN) sat = true;
    }

    out.ax = v[0] / accelLsbPerG_;
    out.ay = v[1] / accelLsbPerG_;
    out.az = v[2] / accelLsbPerG_;
    out.tempC = v[3] / 340.0f + 36.53f;
    out.gx = v[4] / gyroLsbPerDps_;
    out.gy = v[5] / gyroLsbPerDps_;
    out.gz = v[6] / gyroLsbPerDps_;
    out.tUs = t;
    out.saturated = sat;
    return true;
}

// Conta a falha e, a cada maxFailStreak falhas seguidas, tenta recuperar barramento e chip
void Mpu6050::handleFail() {
    failTotal_++;
    failStreak_++;
    if (failStreak_ % cfg_.maxFailStreak != 0) return;
    needsVerify_ = true;
    recoverBus();
    if (configure() == Mpu6050Status::Ok) {
        recoveries_++;
        failStreak_ = 0;
        needsVerify_ = false;
    }
}

// Libera um escravo que ficou segurando o SDA em nível baixo (até 9 pulsos de SCL e um STOP)
// e reinicia o periférico I2C do ESP32
void Mpu6050::recoverBus() {
    cfg_.wire->end();
    pinMode(cfg_.pinSda, INPUT_PULLUP);
    pinMode(cfg_.pinScl, OUTPUT_OPEN_DRAIN);
    digitalWrite(cfg_.pinScl, HIGH);
    for (int i = 0; i < 9 && digitalRead(cfg_.pinSda) == LOW; i++) {
        digitalWrite(cfg_.pinScl, LOW);
        delayMicroseconds(5);
        digitalWrite(cfg_.pinScl, HIGH);
        delayMicroseconds(5);
    }
    pinMode(cfg_.pinSda, OUTPUT_OPEN_DRAIN);
    digitalWrite(cfg_.pinSda, LOW);
    delayMicroseconds(5);
    digitalWrite(cfg_.pinSda, HIGH);   // STOP: SDA sobe com SCL alto
    delayMicroseconds(5);
    cfg_.wire->begin(cfg_.pinSda, cfg_.pinScl, cfg_.i2cFreqHz);
}

bool Mpu6050::readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(cfg_.addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
    return true;
}

bool Mpu6050::writeReg(uint8_t reg, uint8_t val) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    w.write(val);
    return w.endTransmission() == 0;
}

bool Mpu6050::writeVerify(uint8_t reg, uint8_t val) {
    uint8_t back;
    return writeReg(reg, val) && readRegs(reg, &back, 1) && back == val;
}
