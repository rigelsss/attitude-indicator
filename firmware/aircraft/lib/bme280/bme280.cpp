// bme280.cpp — implementação do driver. Registradores conforme o datasheet BST-BME280-DS001 (rev. 1.23).
#include "bme280.h"

// Registradores
static const uint8_t REG_CALIB_TP  = 0x88;   // 24 bytes: dig_T1..dig_P9
static const uint8_t REG_CHIP_ID   = 0xD0;
static const uint8_t REG_RESET     = 0xE0;
static const uint8_t REG_CTRL_HUM  = 0xF2;   // vale só depois de escrever ctrl_meas
static const uint8_t REG_STATUS    = 0xF3;   // bit 0: im_update (cópia dos coeficientes em andamento)
static const uint8_t REG_CTRL_MEAS = 0xF4;   // osrs_t[7:5], osrs_p[4:2], mode[1:0]
static const uint8_t REG_CONFIG    = 0xF5;   // t_sb[7:5], filter[4:2]; escrita garantida só em sleep

static const uint8_t CHIP_ID_BME280 = 0x60;
static const uint8_t CHIP_ID_BMP280 = 0x58;   // mesmos registradores de temperatura e pressão, sem umidade
static const uint8_t RESET_CMD      = 0xB6;
static const uint8_t MODE_NORMAL    = 0x03;
static const uint8_t CONFIG_MASK    = 0xFD;   // bit 1 do config é reservado
static const uint32_t STARTUP_MS    = 2;      // tempo de partida após o reset (datasheet)
static const uint32_t RESET_TIMEOUT_MS = 10;

// Leitura em rajada a partir de ctrl_meas: ctrl_meas, config, (reservado), pressão (3), temperatura (3).
// Confere a configuração junto com os dados, na mesma transação.
static const uint8_t BURST_LEN = 9;

const char *bme280StatusText(Bme280Status s) {
    switch (s) {
    case Bme280Status::Ok:           return "ok";
    case Bme280Status::BadParam:     return "parâmetro de configuração não suportado (pelo chip detectado)";
    case Bme280Status::NoAck:        return "sem resposta no endereço I2C";
    case Bme280Status::WrongId:      return "chip_id inesperado (aceitos: 0x60 BME280, 0x58 BMP280)";
    case Bme280Status::ResetTimeout: return "reset não terminou";
    case Bme280Status::BadCalib:     return "coeficientes de fábrica inválidos";
    case Bme280Status::ConfigFail:   return "registrador não confirmado na releitura";
    }
    return "desconhecido";
}

// Converte os parâmetros nos códigos dos registradores; false se não suportado
static bool osrsCode(uint8_t factor, uint8_t &code) {
    switch (factor) {
    case 1:  code = 1; return true;
    case 2:  code = 2; return true;
    case 4:  code = 3; return true;
    case 8:  code = 4; return true;
    case 16: code = 5; return true;
    }
    return false;
}

static bool iirCode(uint8_t coef, uint8_t &code) {
    switch (coef) {
    case 0:  code = 0; return true;
    case 2:  code = 1; return true;
    case 4:  code = 2; return true;
    case 8:  code = 3; return true;
    case 16: code = 4; return true;
    }
    return false;
}

// t_sb: os códigos 0 a 5 são iguais nos dois chips; 6 e 7 valem 10 e 20 ms no BME280 (datasheet do
// BME280, tabela 27) e 2 e 4 s no BMP280 (datasheet do BMP280, tabela 11)
static bool standbyCode(uint32_t us, bool bmp280, uint8_t &code) {
    switch (us) {
    case 500:     code = 0; return true;
    case 62500:   code = 1; return true;
    case 125000:  code = 2; return true;
    case 250000:  code = 3; return true;
    case 500000:  code = 4; return true;
    case 1000000: code = 5; return true;
    case 10000:   code = 6; return !bmp280;
    case 20000:   code = 7; return !bmp280;
    case 2000000: code = 6; return bmp280;
    case 4000000: code = 7; return bmp280;
    }
    return false;
}

Bme280Status Bme280::begin(const Bme280Config &cfg) {
    cfg_ = cfg;
    configured_ = false;
    uint8_t t, p, sb;
    if (cfg_.wire == nullptr || cfg_.maxFailStreak == 0) return Bme280Status::BadParam;
    // A espera entre medições só é conferida para o modelo em configure(), depois de ler o chip_id;
    // aqui basta ela existir em algum dos dois chips
    if (!osrsCode(cfg_.osrsT, t) || !osrsCode(cfg_.osrsP, p) || !iirCode(cfg_.iirCoef, iirCode_) ||
        !(standbyCode(cfg_.standbyUs, false, sb) || standbyCode(cfg_.standbyUs, true, sb)))
        return Bme280Status::BadParam;
    ctrlMeas_ = (uint8_t)((t << 5) | (p << 2) | MODE_NORMAL);
    // Tolerância ao "não medido" logo após configurar: duas medições no tempo máximo do datasheet
    // (apêndice B: 1,25 + 2,3·osrs_t + 2,3·osrs_p + 0,575 ms, umidade desligada) mais a espera entre elas
    const float measMaxMs = 1.25f + 2.3f * cfg_.osrsT + 2.3f * cfg_.osrsP + 0.575f;
    skipGraceMs_ = (uint32_t)(2.0f * measMaxMs + cfg_.standbyUs / 1000.0f) + 1;

    failStreak_ = 0;
    return reconfigure();
}

Bme280Status Bme280::reconfigure() {
    const Bme280Status st = configure();
    configured_ = (st == Bme280Status::Ok);
    if (configured_) configuredAtMs_ = millis();
    return st;
}

Bme280Status Bme280::configure() {
    if (!readRegs(REG_CHIP_ID, &chipId_, 1)) return Bme280Status::NoAck;
    if (chipId_ != CHIP_ID_BME280 && chipId_ != CHIP_ID_BMP280) return Bme280Status::WrongId;
    uint8_t sb;
    if (!standbyCode(cfg_.standbyUs, chipId_ == CHIP_ID_BMP280, sb)) return Bme280Status::BadParam;
    config_ = (uint8_t)((sb << 5) | (iirCode_ << 2));

    // Reset completo: tudo volta ao padrão (sleep, filtro desligado) e o chip é configurado do zero.
    // Em sleep a escrita do config é garantida; em modo normal ela pode ser ignorada.
    if (!writeReg(REG_RESET, RESET_CMD)) return Bme280Status::ConfigFail;
    delay(STARTUP_MS);
    const uint32_t t0 = millis();
    uint8_t st;
    do {
        if (!readRegs(REG_STATUS, &st, 1)) return Bme280Status::NoAck;
        if (millis() - t0 > RESET_TIMEOUT_MS) return Bme280Status::ResetTimeout;
    } while (st & 0x01);

    uint8_t raw[BME280_CALIB_TP_LEN];
    if (!readRegs(REG_CALIB_TP, raw, sizeof(raw))) return Bme280Status::NoAck;
    if (!bme280ParseCalib(raw, calib_)) return Bme280Status::BadCalib;

    // BME280: ctrl_hum = 0, umidade desligada (só vale depois da escrita do ctrl_meas).
    // O BMP280 não tem o registrador 0xF2: a escrita é pulada.
    if ((chipId_ == CHIP_ID_BME280 && !writeReg(REG_CTRL_HUM, 0x00)) || !writeReg(REG_CONFIG, config_) ||
        !writeReg(REG_CTRL_MEAS, ctrlMeas_))
        return Bme280Status::ConfigFail;
    uint8_t back[2];
    if (!readRegs(REG_CTRL_MEAS, back, 2)) return Bme280Status::NoAck;
    if (back[0] != ctrlMeas_ || (back[1] & CONFIG_MASK) != config_) return Bme280Status::ConfigFail;
    return Bme280Status::Ok;
}

// failStreak_ só é zerado com uma amostra válida: reconfigurar não basta para ok() voltar
const char *Bme280::model() const {
    return chipId_ == CHIP_ID_BME280 ? "BME280" : (chipId_ == CHIP_ID_BMP280 ? "BMP280" : "BME280/BMP280");
}

bool Bme280::read(BaroSample &out) {
    if (!configured_) {
        // Configuração anterior incompleta (ex.: falha só na releitura final): tenta de novo.
        // Com ok() falso, quem usa o driver espaça as chamadas (BARO_FAIL_RETRY_MS no main)
        if (reconfigure() != Bme280Status::Ok) handleFail();
        return false;   // primeira medição ainda não pronta
    }
    uint8_t b[BURST_LEN];
    if (!readRegs(REG_CTRL_MEAS, b, sizeof(b))) {
        handleFail();
        return false;
    }
    if (b[0] != ctrlMeas_ || (b[1] & CONFIG_MASK) != config_) {
        // O chip responde, mas sem a configuração: reiniciou (queda de alimentação) e está em sleep.
        // A recuperação também conta como falha: um chip que reinicia sem parar (VCC sem contato,
        // alimentado só pelos pull-ups do I2C) derruba ok() depois de maxFailStreak recuperações, e
        // quem usa o driver passa a espaçar as tentativas, em vez de um reset completo a cada leitura.
        // Uma queda isolada não derruba ok(): a primeira amostra válida zera a sequência.
        recoveries_++;
        handleFail();
        reconfigure();
        return false;
    }
    int32_t adcP, adcT;
    bme280ParseAdc(b + 3, adcP, adcT);
    float t, p;
    const BaroComp c = bme280Compensate(calib_, adcT, adcP, t, p);
    if (c == BaroComp::Skipped) {
        // Normal só logo após configurar, antes da primeira medição; depois disso é falha
        if (millis() - configuredAtMs_ > skipGraceMs_) handleFail();
        return false;
    }
    if (c != BaroComp::Ok) {   // Invalid: coeficientes inconsistentes
        handleFail();
        return false;
    }
    failStreak_ = 0;
    out.tempC = t;
    out.pressHpa = p;
    out.tMs = millis();
    return true;
}

void Bme280::handleFail() {
    failTotal_++;
    failStreak_++;
}

bool Bme280::readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(cfg_.addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
    return true;
}

bool Bme280::writeReg(uint8_t reg, uint8_t val) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    w.write(val);
    return w.endTransmission() == 0;
}
