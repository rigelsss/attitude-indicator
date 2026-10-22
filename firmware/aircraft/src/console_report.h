// console_report.h — relatórios do console, copiados na sensor_task e formatados na serial_task
// (src/console_view.cpp): a formatação lê só estas cópias, nunca os serviços (refatoracao.md, pendência 7;
// docs/tasks.md). Todos são copiáveis byte a byte (vão pela fila de relatórios, src/report_queue.h).
#pragma once
#include <stdint.h>
#include <type_traits>
#include "acq.h"
#include "attitude.h"
#include "fusion.h"
#include "imu_store.h"
#include "level_store.h"
#include "mag_store.h"
#include "telemetry_flags.h"
#include "baro_service.h"
#include "mag_service.h"
#include "tasks/radio_task.h"
#include "tasks/status_task.h"

enum class ReportKind : uint8_t {
    Window,   // r: uma janela de 1 s fechou
    Att,      // a: uma linha do fluxo de atitude
    Status,   // c: estado
};

// Contadores do MPU e da fila do INT, no instante da cópia
struct MpuCounters {
    uint32_t failTotal, failStreak, recoveries;
    uint32_t intOverflow;   // pulsos que não couberam na fila do INT, desde o boot
};

// Resumo por segundo (r)
struct WindowReport {
    SampleWindow w;
    uint32_t     windowUs;
    MpuCounters  mpu;
    BaroSummary  baro;
    MagSummary   mag;
    RadioCounters radio;       // contadores do rádio no fim da janela
    RadioCounters radioPrev;   // e no fim da janela anterior (taxas por segundo)
};

// Uma linha do fluxo de atitude (a), com a estatística da aquisição e do filtro desde a linha anterior
struct AttReport {
    bool        initialized;      // filtro inicializado
    float       rollDeg, pitchDeg;
    bool        degraded;
    float       accWeight;        // peso do acelerômetro na última amostra
    Angles      accOnly;          // só do acelerômetro
    bool        gyroOnlyInit;     // diagnóstico só de giroscópio
    float       gyroOnlyRoll, gyroOnlyPitch;
    float       accNorm;          // |a| corrigido, nos eixos do modelo
    float       tempC;
    AcqStats    acq;
    FusionStats fusion;
};

// Estado (c). validity é capturado no pedido, antes da cópia do resto (as idades valem para esse instante)
struct StatusReport {
    ImuCalData     imu;
    LevelData      level;
    bool           attInitialized;
    float          rollDeg, pitchDeg;
    bool           attDegraded;
    Vec3           gyroBias;
    float          gyroBiasTempC;
    bool           gyroDefinitive;
    bool           zeroValid, zeroActive;
    float          zeroRefHpa, zeroRefTempC;
    BaroSummary    baro;
    MagCalData     magCal;
    MagSummary     mag;
    ValidityInputs validity;
    SampleWindow   last;          // última janela de 1 s
    uint32_t       intOverflow;
    RadioCounters  radio;         // desde o boot
    StatusCounters oled;          // OLED de status, desde o boot
};

static_assert(std::is_trivially_copyable<WindowReport>::value, "WindowReport vai pela fila byte a byte");
static_assert(std::is_trivially_copyable<AttReport>::value, "AttReport vai pela fila byte a byte");
static_assert(std::is_trivially_copyable<StatusReport>::value, "StatusReport vai pela fila byte a byte");
