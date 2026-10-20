// baro_service.cpp — ver baro_service.h
#include "baro_service.h"
#include <math.h>
#include "logger.h"

BaroService::BaroService(const BaroServiceConfig &cfg)
    : cfg_(cfg), periodUs_(1000000UL / cfg.readHz), printUs_(1000000UL / cfg.printHz), zero_(cfg.zero) {}

void BaroService::begin() { start(); }

void BaroService::start() {
    const Bme280Status st = baro_.begin(cfg_.chip);
    if (st != Bme280Status::Ok) {
        if (st != lastStatus_)
            logPrintf("[baro] BME280/BMP280: %s (chip_id=0x%02X); sem altitude, nova tentativa a cada %lu s\n",
                      bme280StatusText(st), baro_.chipId(), (unsigned long)(cfg_.retryMs / 1000));
        lastStatus_ = st;
        retryAtMs_ = millis() + cfg_.retryMs;
        return;
    }
    lastStatus_ = st;
    ready_ = true;
    lastRecoveries_ = baro_.recoveries();
    sched_.start(micros());
    logPrintf("[baro] %s configurado (chip_id=0x%02X): pressão x%d, temperatura x%d, IIR %d, modo normal\n",
              baro_.model(), baro_.chipId(), cfg_.chip.osrsP, cfg_.chip.osrsT, cfg_.chip.iirCoef);
    startZero(true);
}

void BaroService::startZero(bool settle) {
    zero_.start(settle, millis());
    if (settle)
        logPrintf("[baro] zero da altitude: aguardando %lu ms o filtro do chip e depois %lu ms com a caixa "
                  "parada...\n", (unsigned long)cfg_.zero.settleMs, (unsigned long)cfg_.zero.windowMs);
    else
        logPrintf("[baro] zero da altitude: medindo %lu ms, mantenha a caixa parada...\n",
                  (unsigned long)cfg_.zero.windowMs);
}

void BaroService::onZeroEvent(BaroZero::Event ev) {
    if (ev == BaroZero::Event::None) return;
    const BaroZeroResult &r = zero_.lastResult();
    if (ev == BaroZero::Event::Accepted) {
        tempWarned_ = false;
        logPrintf("[baro] zero aceito: p0 = %.2f hPa | desvio %.2f Pa | %lu leituras | %.1f °C\n", r.refHpa,
                  r.stdPa, (unsigned long)zero_.lastCount(), r.tempC);
        return;
    }
    if (r.status == BaroZeroCheck::Unstable)
        logPrintf("[baro] zero recusado: desvio %.2f Pa acima de %.1f Pa (caixa mexida ou ar agitado); "
                  "medindo de novo\n", r.stdPa, cfg_.zero.maxStdPa);
    else if (r.status == BaroZeroCheck::Invalid)
        logPrintln("[baro] zero recusado: leitura inválida (não finita) na janela; medindo de novo");
    else if (r.status == BaroZeroCheck::OutOfRange)
        logPrintf("[baro] zero recusado: p0 = %.2f hPa fora de %.0f–%.0f hPa (leitura absurda); medindo de novo\n",
                  r.refHpa, cfg_.zero.minHpa, cfg_.zero.maxHpa);
    else
        logPrintf("[baro] zero recusado: %lu leituras, mínimo %lu; medindo de novo\n",
                  (unsigned long)zero_.lastCount(), (unsigned long)cfg_.zero.minSamples);
}

float BaroService::altitudeM() const {
    return have_ && zero_.refValid() ? baroAltitudeM(last_.pressHpa, zero_.refHpa()) : NAN;
}

void BaroService::printLine(const BaroSample &b) const {
    const float alt = zero_.refValid() ? baroAltitudeM(b.pressHpa, zero_.refHpa()) : NAN;
    if (isfinite(alt))
        logPrintf("[baro] %6.2f °C | %8.2f hPa | alt %+6.2f m | ΔT %+5.2f °C desde o zero\n", b.tempC, b.pressHpa,
                  alt, b.tempC - zero_.refTempC());
    else if (zero_.refValid())
        logPrintf("[baro] %6.2f °C | %8.2f hPa | alt: inválida (pressão ou zero fora da faixa)\n", b.tempC,
                  b.pressHpa);
    else
        logPrintf("[baro] %6.2f °C | %8.2f hPa | alt: sem zero\n", b.tempC, b.pressHpa);
}

void BaroService::printSummary() const {
    if (!ready_) {
        logPrintf("    baro: barômetro indisponível (%s)\n", bme280StatusText(lastStatus_));
        return;
    }
    logPrintf("    baro: %s | ", baro_.ok() ? "ok" : "FALHANDO");
    const float alt = altitudeM();
    if (!have_) logPrint("sem leitura");
    else if (isfinite(alt))
        logPrintf("%.2f hPa | alt %+.2f m | %.1f °C (ΔT %+.1f desde o zero)", last_.pressHpa, alt, last_.tempC,
                  last_.tempC - zero_.refTempC());
    else if (zero_.refValid())
        logPrintf("%.2f hPa | alt inválida | %.1f °C", last_.pressHpa, last_.tempC);
    else logPrintf("%.2f hPa | sem zero | %.1f °C", last_.pressHpa, last_.tempC);
    logPrintf(" | falhas %lu (seguidas %lu, recuperações %lu)\n", (unsigned long)baro_.failTotal(),
              (unsigned long)baro_.failStreak(), (unsigned long)baro_.recoveries());
}

void BaroService::service(uint32_t nowUs) {
    if (!ready_) {
        if ((int32_t)(millis() - retryAtMs_) >= 0) start();
        return;
    }
    if (!sched_.due(nowUs)) return;

    BaroSample b;
    const bool ok = baro_.read(b);
    // Com o chip falhando, tenta só a cada failRetryMs: cada transação sem resposta imprime um erro do
    // I2C e atrasa o laço, e o MPU descartava amostras (hardware.md, 12/10/2026)
    sched_.advance(nowUs, baro_.ok() ? periodUs_ : cfg_.failRetryMs * 1000UL);
    if (baro_.recoveries() != lastRecoveries_) {
        lastRecoveries_ = baro_.recoveries();
        logPrintf("[baro] %s perdeu a configuração (queda de alimentação?): %s\n", baro_.model(),
                  baro_.configured() ? "reconfigurado; o filtro do chip recomeçou"
                                     : "reconfiguração falhou, nova tentativa a cada leitura");
        if (zero_.active()) startZero(true);
    }
    if (!ok) return;
    last_ = b;
    have_ = true;
    onZeroEvent(zero_.feed(b.pressHpa, b.tempC, b.tMs));
    if (zero_.refValid() && !tempWarned_ && fabsf(b.tempC - zero_.refTempC()) > cfg_.tempWarnC) {
        logPrintf("[baro] AVISO: chip a %.1f °C, zero feito a %.1f °C: a altitude deriva ~13 cm/°C. "
                  "Zere de novo com 0\n", b.tempC, zero_.refTempC());
        tempWarned_ = true;
    }
    if (streamOn_ && streamPrint_.due(nowUs)) {
        streamPrint_.restart(nowUs, printUs_);
        printLine(b);
    }
}

void BaroService::zeroAltitude() {
    if (!ready_) logPrintln("[baro] barômetro indisponível: sem altitude");
    else if (zero_.state() == BaroZero::State::Settling) logPrintln("[baro] zero já em andamento");
    else startZero(false);
}

void BaroService::toggleStream(uint32_t nowUs) {
    streamOn_ = !streamOn_;
    streamPrint_.start(nowUs);
    logPrintf("[baro] fluxo de altitude %s (%lu Hz)\n", streamOn_ ? "ligado" : "desligado",
              (unsigned long)cfg_.printHz);
}
