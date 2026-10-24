// sensor_task.h — task dona do barramento de sensores (Wire): acordada pela INT do MPU a SAMPLE_RATE_HZ,
// lê o MPU6050 e, intercalados, o BME280 e o HMC5883L; calibração e fusão (roll, pitch, proa).
// Núcleo SENSOR_TASK_CORE (1), prioridade SENSOR_TASK_PRIO (config.h). docs/plano.md, semana 3.
// Comandos de outras tasks chegam por uma fila e são executados aqui, entre duas leituras: é a camada única
// de comandos (hoje as teclas do console; na semana 5, também os CMD_* do B). refatoracao.md, pendência 7.
#pragma once
#include <stdint.h>
#include "telemetry_pack.h"

enum CmdSource : uint8_t {
    CMD_SRC_CONSOLE = 0,   // tecla da serial
    CMD_SRC_RADIO = 1,     // CommandPacket do B (docs/web.md): cmd, arg e seq
};

struct SensorCmd {
    uint8_t  src;   // CmdSource
    char     key;   // tecla do console
    uint8_t  cmd;   // rádio: CommandId
    int32_t  arg;   // rádio: argumento (CMD_SET_RATE_HZ)
    uint16_t seq;   // rádio: seq do CommandPacket (casa o ACK)
};

// Cria a fila de comandos e a task (que configura o I2C e os sensores no próprio núcleo). false sem memória
bool sensorTaskStart();
// Enfileira um comando, sem esperar. false com a fila cheia ou antes do sensorTaskStart()
bool sensorTaskSend(const SensorCmd &c);
// Última cópia do estado, publicada a cada ciclo da sensor_task (caixa de correio: fila de 1, sobrescrita).
// false antes da primeira publicação. Usado pela radio_task
bool sensorLatestState(TelemetryState &out);

#ifdef PULSE_TEST
// Só no ambiente aircraft_pulsetest (src/pulse_test.h): põe um instante falso na fila do INT, como a ISR faria.
// false com a fila cheia (contado por quem injeta, fora do contador "fila cheia" da ISR)
bool sensorInjectPulse(uint32_t tUs);
#endif
