// alarm_logic.h — condições de alarme do ESP B e padrões dos LEDs (docs/solo.md, passo 1).
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Condições, em ordem de prioridade:
//   1. link perdido: sem pacote aceito há mais que o timeout, depois de o link ter funcionado uma vez (antes do
//      primeiro pacote não há alarme; o LED do link apagado já mostra a situação);
//   1b. dado atrasado: o link está vivo, mas a idade estimada do dado (atraso na chegada + tempo desde a
//      chegada; lib/delay_estimator) passa do limite; o dado na tela não é atual, como no link perdido (mesmo ritmo do LED);
//   2. atitude inválida: link OK, VAL_ATT desligada por mais de invalidDelayMs, fora de uma calibração
//      (ST_CALIBRATING): no boot do A e nas calibrações a atitude fica inválida por instantes, o que é esperado;
//   3. alarme de atitude anormal (warning): |roll| > bankWarningDeg, pitch > pitchUpDeg ou pitch < −pitchDownDeg;
//   4. aviso de bank (caution): |roll| > bankCautionDeg.
// Os limites de atitude entram na hora e saem com hystDeg a menos (sem histerese, a caixa parada perto do limite
// ligaria e desligaria o alarme com o ruído do ângulo). Só são avaliados com o link OK e a atitude válida.
// VAL_ATT ligada com roll ou pitch não finito (pacote incoerente) conta como atitude inválida.
// As piscadas contam a fase a partir da mudança de condição e começam acesas: um alarme novo aparece na hora.
// Reconhecer (botão do encoder ou tecla k) deixa o LED aceso fixo enquanto a mesma condição durar; uma condição
// diferente, ou a mesma voltando depois de terminar, alarma de novo.
#pragma once
#include <stdint.h>

struct AlarmConfig {
    float    bankCautionDeg, bankWarningDeg, pitchUpDeg, pitchDownDeg, hystDeg;
    uint32_t invalidDelayMs;
    // Padrões do LED de alarme (ms)
    uint32_t linkBlinkMs, cautionBlinkMs, warningBlinkMs, doublePeriodMs, doubleOnMs;
};

struct AlarmInputs {
    bool  everLinked;    // já chegou pelo menos um pacote aceito
    bool  linkOk;        // o último pacote aceito tem no máximo o timeout de idade
    bool  dataLate;      // a idade estimada do dado (dataAgeMs) passa de LINK_STALE_DELAY_MS
    bool  attValid;      // VAL_ATT do último pacote
    bool  calibrating;   // ST_CALIBRATING do último pacote
    float rollDeg, pitchDeg;
};

// Em ordem crescente de prioridade
enum class AlarmKind : uint8_t { None, BankCaution, AttitudeWarning, AttInvalid, DataLate, LinkLost };
const char *alarmKindText(AlarmKind k);
// Versão curta, só ASCII, até 13 caracteres (OLED: "ALARME " + texto cabe em 21)
const char *alarmKindShort(AlarmKind k);

struct AlarmOutput {
    AlarmKind kind;
    bool      acked;
    bool      alarmLed;   // LED de alarme (e buzzer) neste instante
    bool      linkLed;    // LED do link: aceso com o link OK
};

class AlarmLogic {
public:
    explicit AlarmLogic(const AlarmConfig &cfg);
    AlarmOutput update(const AlarmInputs &in, uint32_t nowMs);
    // Reconhece a condição ativa. false se não há condição
    bool acknowledge();
    AlarmKind kind() const { return kind_; }
    bool acked() const { return acked_; }

private:
    bool ledOn(uint32_t nowMs) const;

    AlarmConfig cfg_;
    bool        caution_ = false, warning_ = false;
    bool        invalidTiming_ = false;
    uint32_t    invalidSinceMs_ = 0;
    AlarmKind   kind_ = AlarmKind::None;
    uint32_t    kindSinceMs_ = 0;   // instante da última mudança de condição (fase das piscadas)
    bool        acked_ = false;
};
