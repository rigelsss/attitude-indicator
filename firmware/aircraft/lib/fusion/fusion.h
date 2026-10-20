// fusion.h — núcleo da fusão em lógica pura (sem Arduino.h), testável com pio test -e native.
// Junta o filtro de atitude (lib/attitude), o diagnóstico só de giroscópio e a predição da proa
// (HeadingFilter, lib/mag), com o dt medido pelos instantes dos pulsos de "data ready".
// Fica fora do lib/attitude porque o lib/mag já depende dele. Docs: docs/fusao.md e docs/refatoracao.md.
#pragma once
#include <stdint.h>
#include "attitude.h"
#include "mag.h"

// Estatística do filtro entre duas linhas do fluxo a (só as amostras com Step::Updated, menos os reinícios)
struct FusionStats {
    uint32_t n = 0;             // intervalos integrados
    float    dtMin = 1e9f, dtMax = 0;
    double   dtSum = 0;
    uint32_t costSumUs = 0;     // custo de AttitudeFilter::update() (só o filtro): soma e máximo
    uint32_t costMaxUs = 0;
    uint32_t resets = 0;        // reinícios por intervalo acima de maxDtS
};

class FusionCore {
public:
    FusionCore(const FusionParams &fp, const HeadingParams &hp);

    // Relógio em µs para medir o custo do filtro (no firmware, micros()). Sem relógio, o custo é 0.
    void setClock(uint32_t (*nowUs)()) { clock_ = nowUs; }

    // Uma amostra válida: acc e gyro nos eixos do modelo (imuToBody), intUs o instante do pulso de
    // "data ready" desta amostra. headingHold: a proa fica parada e recomeça depois (calibração do
    // magnetômetro em andamento). A primeira amostra depois de reset() só marca a referência do dt.
    AttitudeFilter::Step update(const Vec3 &acc, const Vec3 &gyro, const ImuFlags &flags, uint32_t intUs,
                                bool headingHold);
    // Reinício (lacuna na aquisição, MPU reconfigurado, calibração nova): atitude, diagnóstico, proa e dt
    void reset();
    // O diagnóstico só de giroscópio recomeça da atitude atual (comando a)
    void resetGyroOnly() { gyroOnly_.reset(); }

    const AttitudeFilter &att() const { return att_; }
    const AttitudeFilter &gyroOnly() const { return gyroOnly_; }
    HeadingFilter &heading() { return hdg_; }
    const HeadingFilter &heading() const { return hdg_; }
    bool havePrev() const { return prevValid_; }       // há um pulso anterior aceito (referência do dt)
    uint32_t prevIntUs() const { return prevIntUs_; }

    const FusionStats &stats() const { return stats_; }
    void resetStats() { stats_ = FusionStats(); }

private:
    AttitudeFilter att_;
    AttitudeFilter gyroOnly_;   // diagnóstico: deriva só do giroscópio
    HeadingFilter  hdg_;
    bool     prevValid_ = false;
    uint32_t prevIntUs_ = 0;
    FusionStats stats_;
    uint32_t (*clock_)() = nullptr;
};
