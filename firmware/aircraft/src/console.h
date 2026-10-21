// console.h — console serial do ESP A, lado da sensor_task: tecla → comando, liga e desliga do resumo por
// segundo (r) e do fluxo de atitude (a), e cópia do estado (c) e das linhas do r e do a para a serial_task, que
// as formata (src/console_view.cpp). As teclas chegam pela fila de comandos (src/tasks/sensor_task.h). Este lado
// pode chamar os serviços direto, porque roda na task dona deles; a formatação, na serial_task, lê só as
// cópias (refatoracao.md, pendência 7; docs/tasks.md). Imprime pelo lib/logger.
#pragma once
#include <Arduino.h>
#include "mpu6050.h"
#include "acq.h"
#include "fusion.h"
#include "cal_service.h"
#include "baro_service.h"
#include "mag_service.h"
#include "telemetry_flags.h"
#include "tasks/radio_task.h"

// O que o console lê e comanda, montado na sensor_task
struct ConsoleDeps {
    Mpu6050 &mpu;                          // contadores de falha
    const volatile uint32_t &intOverflow;  // pulsos que não couberam na fila do INT, desde o boot
    const PulseTracker &acq;               // janelas de 1 s (r e c)
    AcqStats &acqStats;                    // estatística da aquisição entre duas linhas do a
    FusionCore &fusion;
    CalService &cal;
    BaroService &baro;
    MagService &mag;
    ValidityInputs (*validityInputs)();    // entradas do campo valid, no instante da chamada
    void (*fusionReset)();
};

class Console {
public:
    explicit Console(const ConsoleDeps &d) : d_(d) {}
    void printHelp() const;
    // Uma tecla: calibração do magnetômetro, seis posições e montagem primeiro; depois os comandos
    void handleKey(char ch);
    // Uma janela de 1 s fechou (antes de ser guardada como a última): cópia para o resumo, se o r estiver ligado
    void windowClosed(const SampleWindow &w) const;
    // Uma amostra passou pela fusão: cópia da linha do a quando vence (acc nos eixos do modelo, já corrigido)
    void attitudeSample(const Vec3 &acc, float tempC, uint32_t intUs);

private:
    void sendStatus() const;
    void toggleAttitude();

    ConsoleDeps d_;
    mutable RadioCounters lastRadio_ = {};   // contadores do rádio no fim da janela anterior (taxas do r)
    bool summaryOn_ = false;   // resumo por segundo (r)
    bool attOn_ = false;       // fluxo de atitude (a)
    Periodic attPrint_;        // próxima linha do a
};
