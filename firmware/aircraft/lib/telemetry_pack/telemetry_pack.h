// telemetry_pack.h — estado publicado pela sensor_task e montagem do TelemetryPacket pela radio_task
// (docs/radio.md); decodificação do CommandPacket recebido do B. Lógica pura (sem Arduino.h), testável com
// pio test -e native. O contrato é o firmware/shared/include/telemetry.h (v4).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "telemetry.h"
#include "telemetry_flags.h"

// Cópia do estado, gravada pela sensor_task a cada ciclo numa caixa de correio (fila de 1, sobrescrita) e
// lida pela radio_task. Leva os instantes das últimas amostras, e não as idades: elas são calculadas na
// montagem do pacote (validityAt)
struct TelemetryState {
    // Atitude e proa
    bool     attInitialized, attDegraded;
    float    rollDeg, pitchDeg;
    bool     haveAttSample;     // há um pulso do MPU usado na fusão
    uint64_t attSampleUs64;     // instante (µs, 64 bits, relógio do esp_timer) desse pulso: sem virada, um
                                // ângulo velho não volta a valer depois de ~71,6 min com o MPU parado
    bool     hdgInitialized;
    float    headingDeg;        // magnética, 0..360
    float    gx, gy, gz;        // °/s, eixos do modelo, corrigidos (bias e montagem)
    // Barômetro (instantes em millis() de 32 bits: a virada, em ~49,7 dias, fica fora de qualquer uso)
    bool     baroOk, baroZeroValid, haveBaro;
    uint32_t baroMs;            // instante (ms) da última leitura válida
    float    altitudeM;         // NAN sem zero ou fora da faixa
    float    pressureHpa, tempC;
    // Magnetômetro
    bool     magOk, magCalibrated, hdgMagSuspended, haveMag;
    uint32_t magMs;             // instante (ms) da última leitura válida
    // Saúde e calibração (campo status)
    bool     mpuOk;
    bool     calibrating;       // giroscópio, seis posições, montagem ou magnetômetro em curso
    bool     gyroCalibrated;    // bias medido neste boot
    // Pulsos falsos no INT desde o boot (só para o OLED de status; não vai no pacote)
    uint32_t intSpurious;
    // Latência da última amostra fundida: pulso do INT → fim da fusão (loop_us)
    uint32_t loopUs;
};

// Entradas do campo valid no instante now (idades a partir dos instantes do estado). nowUs64: o mesmo relógio
// do attSampleUs64 (esp_timer_get_time())
ValidityInputs validityAt(const TelemetryState &s, uint32_t nowMs, uint64_t nowUs64);
// Campo status (StatusFlags)
uint16_t telemetryStatusFlags(const TelemetryState &s);
// Pacote completo: cabeçalho (magic, versão, tipo, seq), campos, status, valid e reservas em 0
TelemetryPacket buildTelemetry(const TelemetryState &s, uint32_t nowMs, uint64_t nowUs64, uint16_t seq,
                               const ValidityLimits &lim);

// Comando recebido do B: o buffer do ESP-NOW é copiado (memcpy) antes de qualquer leitura, porque não tem
// alinhamento garantido (telemetry.h)
enum class CmdDecode : uint8_t { Ok, BadSize, BadMagic, BadVersion, BadType };
CmdDecode decodeCommand(const uint8_t *data, size_t len, CommandPacket &out);
const char *cmdDecodeText(CmdDecode r);

// ACK de um comando (A → B, docs/web.md): cabeçalho com o contador de mensagens do A (msgSeq), o comando, o
// resultado (AckResult) e o seq do CommandPacket confirmado (cmdSeq), com as reservas em 0
AckPacket buildAck(uint16_t msgSeq, uint8_t cmd, uint8_t result, uint16_t cmdSeq);
