// hmc5883l.cpp — implementação do driver. Registradores conforme o datasheet do HMC5883L (Honeywell, rev. E).
#include "hmc5883l.h"

// Registradores
static const uint8_t REG_CRA  = 0x00;   // média, taxa de saída e polarização
static const uint8_t REG_CRB  = 0x01;   // ganho
static const uint8_t REG_MODE = 0x02;
static const uint8_t REG_ID_A = 0x0A;   // 0x0A..0x0C = "H43"

const char *hmc5883lStatusText(Hmc5883lStatus s) {
    switch (s) {
    case Hmc5883lStatus::Ok:         return "ok";
    case Hmc5883lStatus::BadParam:   return "parâmetro de configuração não suportado";
    case Hmc5883lStatus::NoAck:      return "sem resposta no endereço I2C";
    case Hmc5883lStatus::WrongId:    return "ID diferente de \"H43\"";
    case Hmc5883lStatus::ConfigFail: return "registrador não confirmado na releitura";
    }
    return "desconhecido";
}

Hmc5883lStatus Hmc5883l::begin(const Hmc5883lConfig &cfg) {
    cfg_ = cfg;
    configured_ = false;
    if (cfg_.wire == nullptr || cfg_.maxFailStreak == 0) return Hmc5883lStatus::BadParam;
    if (!hmcCraCode(cfg_.avgSamples, cfg_.outputHz, cra_) || !hmcCrbCode(cfg_.rangeMga, crb_, lsbPerGa_))
        return Hmc5883lStatus::BadParam;
    // Duas saídas: a primeira depois de configurar ainda pode ser a medição anterior (ou zero após um reset)
    settleMs_ = (uint32_t)(2000.0f / cfg_.outputHz) + 5;
    failStreak_ = 0;
    return reconfigure();
}

Hmc5883lStatus Hmc5883l::reconfigure() {
    const Hmc5883lStatus st = configure();
    configured_ = (st == Hmc5883lStatus::Ok);
    if (configured_) configuredAtMs_ = millis();
    return st;
}

Hmc5883lStatus Hmc5883l::configure() {
    uint8_t id[3];
    if (!readRegs(REG_ID_A, id, 3)) return Hmc5883lStatus::NoAck;
    if (id[0] != 'H' || id[1] != '4' || id[2] != '3') return Hmc5883lStatus::WrongId;
    if (!writeReg(REG_CRA, cra_) || !writeReg(REG_CRB, crb_) || !writeReg(REG_MODE, HMC_MODE_CONTINUOUS))
        return Hmc5883lStatus::ConfigFail;
    uint8_t back[HMC_BURST_LEN];
    if (!readRegs(REG_CRA, back, HMC_BURST_LEN)) return Hmc5883lStatus::NoAck;
    int16_t c[3];
    if (hmcDecode(back, cra_, crb_, HMC_MODE_CONTINUOUS, c) == HmcFrame::ConfigLost) return Hmc5883lStatus::ConfigFail;
    return Hmc5883lStatus::Ok;
}

// failStreak_ só é zerado com uma amostra válida: reconfigurar não basta para ok() voltar
bool Hmc5883l::read(MagSample &out) {
    if (!configured_) {
        // Configuração anterior incompleta: tenta de novo. Com ok() falso, quem usa o driver espaça
        // as chamadas (MAG_FAIL_RETRY_MS no main)
        if (reconfigure() != Hmc5883lStatus::Ok) handleFail();
        return false;
    }
    uint8_t b[HMC_BURST_LEN];
    if (!readRegs(REG_CRA, b, sizeof(b))) {
        handleFail();
        return false;
    }
    int16_t c[3];
    const HmcFrame f = hmcDecode(b, cra_, crb_, HMC_MODE_CONTINUOUS, c);
    if (f == HmcFrame::ConfigLost) {
        // O chip responde, mas sem a configuração: reiniciou e está em medição única
        recoveries_++;
        if (reconfigure() != Hmc5883lStatus::Ok) handleFail();
        return false;
    }
    if (millis() - configuredAtMs_ < settleMs_) return false;   // primeira medição ainda não pronta
    if (f == HmcFrame::Saturated) {
        saturations_++;   // comunicação ok, mas a amostra não serve
        return false;
    }
    failStreak_ = 0;
    out.chipUt = hmcCountsToUt(c, lsbPerGa_);
    out.tMs = millis();
    return true;
}

void Hmc5883l::handleFail() {
    failTotal_++;
    failStreak_++;
}

bool Hmc5883l::readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(cfg_.addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
    return true;
}

bool Hmc5883l::writeReg(uint8_t reg, uint8_t val) {
    TwoWire &w = *cfg_.wire;
    w.beginTransmission(cfg_.addr);
    w.write(reg);
    w.write(val);
    return w.endTransmission() == 0;
}
