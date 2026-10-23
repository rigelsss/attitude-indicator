// telemetry_rx.h — conferência e cópia do TelemetryPacket recebido do ESP A (docs/radio.md, ESP B).
// Lógica pura (sem Arduino.h), testável com pio test -e native. O contrato é o
// firmware/shared/include/telemetry.h (v4).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "telemetry.h"

// Motivo da recusa, na ordem em que é conferido: o cabeçalho primeiro, para um pacote de outro tipo (um ACK,
// na semana 5) ou de outra versão ser contado pelo motivo certo, e não como tamanho errado
enum class RxDecode : uint8_t { Ok, BadSize, BadMagic, BadVersion, BadType };

// O buffer do ESP-NOW é copiado (memcpy) antes de qualquer leitura, porque não tem alinhamento garantido
// (telemetry.h). out só vale com Ok
RxDecode decodeTelemetry(const uint8_t *data, size_t len, TelemetryPacket &out);
const char *rxDecodeText(RxDecode r);
