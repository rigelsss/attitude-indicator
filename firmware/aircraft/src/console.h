// console.h — console serial do ESP A: tecla → comando, estado (c), resumo por segundo (r), fluxo de
// atitude (a) e ajuda. Só formata e despacha: a lógica fica nos serviços e nas libs puras. Imprime pelo
// lib/logger. Docs: docs/refatoracao.md (passo 8).
// Roda na sensor_task: as teclas chegam pela fila de comandos (src/tasks/sensor_task.h), lidas do Serial pela
// serial_task. Por isso pode chamar os serviços direto. O c, o r e o a ainda são formatados aqui, na
// sensor_task; no passo 2 da semana 3 vão para a serial_task (refatoracao.md, pendências 2 e 7).
#pragma once
#include <Arduino.h>
#include "mpu6050.h"
#include "acq.h"
#include "fusion.h"
#include "cal_service.h"
#include "baro_service.h"
#include "mag_service.h"
#include "telemetry_flags.h"

// O que o console lê e comanda, montado no main.cpp
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
    // Uma janela de 1 s fechou (antes de ser guardada como a última): resumo, se o r estiver ligado
    void windowClosed(const SampleWindow &w) const;
    // Uma amostra passou pela fusão: linha do a quando vence (acc nos eixos do modelo, já corrigido)
    void attitudeSample(const Vec3 &acc, float tempC, uint32_t intUs);

private:
    void printStatus() const;
    void printValidity(const ValidityInputs &in) const;
    void printWindow(const SampleWindow &w) const;
    void printAttitudeLine(const Vec3 &acc, float tempC) const;
    void toggleAttitude();

    ConsoleDeps d_;
    bool summaryOn_ = false;   // resumo por segundo (r)
    bool attOn_ = false;       // fluxo de atitude (a)
    Periodic attPrint_;        // próxima linha do a
};
