// sensor_task.h — task dona do barramento de sensores (Wire): acordada pela INT do MPU a SAMPLE_RATE_HZ,
// lê o MPU6050 e, intercalados, o BME280 e o HMC5883L; calibração e fusão (roll, pitch, proa).
// Núcleo SENSOR_TASK_CORE (1), prioridade SENSOR_TASK_PRIO (config.h). docs/plano.md, semana 3.
// Comandos de outras tasks chegam por uma fila e são executados aqui, entre duas leituras: é a camada única
// de comandos (hoje as teclas do console; na semana 5, também os CMD_* do B). refatoracao.md, pendência 7.
#pragma once
#include <stdint.h>

enum CmdSource : uint8_t {
    CMD_SRC_CONSOLE = 0,   // tecla da serial
};

struct SensorCmd {
    uint8_t src;   // CmdSource
    char    key;   // tecla do console
};

// Cria a fila de comandos e a task (que configura o I2C e os sensores no próprio núcleo). false sem memória
bool sensorTaskStart();
// Enfileira um comando, sem esperar. false com a fila cheia ou antes do sensorTaskStart()
bool sensorTaskSend(const SensorCmd &c);
