// radio_commands.h — execução dos comandos do rádio no ESP A (docs/web.md, passo 1). Roda na sensor_task, que é
// a dona dos serviços: é a camada única de comandos (o mesmo lugar e as mesmas regras das teclas do console;
// item 8 da revisão de 15/10). Cada comando gera um ACK (radioSendAck): ACCEPTED ao começar e DONE ou REJECTED
// ao terminar nos demorados; DONE ou REJECTED na hora nos imediatos. Duplicados, comando em curso e tempo limite
// ficam no lib/cmd_tracker. Imprime pelo lib/logger, com o prefixo [cmd].
//   CMD_CALIBRATE_GYRO  demorado: a mesma calibração do g (caixa parada)
//   CMD_CALIBRATE_MAG   demorado: a mesma coleta do m; na revisão, grava sozinha se passar nos critérios
//   CMD_ZERO_ALTITUDE   demorado: o mesmo zero do 0
//   CMD_ZERO_ATTITUDE   imediato: a atitude atual vira roll = pitch = 0 até o próximo boot (|ângulos| até
//                       ZERO_ATT_MAX_DEG), subtraída só da telemetria. Exige a atitude válida pelo critério da
//                       telemetria (VAL_ATT, com a idade da última amostra, e sem VAL_ATT_DEGRADED)
//   CMD_SET_RATE_HZ     imediato: 10, 20, 25 ou 50 Hz
#pragma once
#include <Arduino.h>
#include "attitude.h"
#include "baro_service.h"
#include "cal_service.h"
#include "cmd_tracker.h"
#include "mag_service.h"
#include "telemetry_flags.h"
#include "tasks/sensor_task.h"

class RadioCommands {
public:
    // validity: entradas do campo valid no instante da chamada (as mesmas da telemetria)
    RadioCommands(CalService &cal, BaroService &baro, MagService &mag, const AttitudeFilter &att,
                  ValidityInputs (*validity)(), const ValidityLimits &limits, uint32_t retentionLongMs,
                  uint32_t retentionShortMs)
        : cal_(cal), baro_(baro), mag_(mag), att_(att), validity_(validity), limits_(limits),
          tracker_(retentionLongMs, retentionShortMs) {}
    // Um comando do rádio, tirado da fila da sensor_task
    void arrive(const SensorCmd &c);
    // A cada ciclo: fim do comando demorado em curso e tempo limite
    void service(uint32_t nowMs);
    // Zero operacional em uso (0 e 0 sem zero), subtraído do roll e do pitch da telemetria
    float zeroRollDeg() const { return zeroRoll_; }
    float zeroPitchDeg() const { return zeroPitch_; }

private:
    void reject(const SensorCmd &c, const char *why);
    void done(const SensorCmd &c);
    void start(const SensorCmd &c, uint32_t timeoutMs);
    void complete(uint8_t result, const char *what);

    CalService &cal_;
    BaroService &baro_;
    MagService &mag_;
    const AttitudeFilter &att_;
    ValidityInputs (*validity_)();
    ValidityLimits limits_;
    CmdTracker tracker_;
    uint32_t gyroOutcomesAtStart_ = 0;
    uint32_t zeroAcceptsAtStart_ = 0;
    float zeroRoll_ = 0.0f, zeroPitch_ = 0.0f;
};

const char *commandName(uint8_t cmd);
