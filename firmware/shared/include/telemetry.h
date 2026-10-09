// telemetry.h — contrato de dados entre ESP A (avião) e ESP B (solo)
// Compartilhado pelos dois projetos via build_flags = -I../shared/include
// Qualquer mudança aqui exige recompilar e regravar OS DOIS firmwares.
#pragma once
#include <stdint.h>

#define TELEMETRY_MAGIC    0xA7   // identifica pacotes do projeto
#define TELEMETRY_VERSION  1      // incrementar ao mudar qualquer struct

// ---------- Tipos de mensagem ----------
enum MsgType : uint8_t {
    MSG_TELEMETRY = 1,   // A -> B, periódica (~50 Hz)
    MSG_COMMAND   = 2,   // B -> A, sob demanda
    MSG_ACK       = 3,   // A -> B, confirmação de comando
};

// ---------- Cabeçalho comum ----------
struct __attribute__((packed)) MsgHeader {
    uint8_t  magic;      // TELEMETRY_MAGIC
    uint8_t  version;    // TELEMETRY_VERSION
    uint8_t  type;       // MsgType
    uint16_t seq;        // contador; B usa para detectar pacotes perdidos
};

// ---------- Flags de status (bitmask) ----------
enum StatusFlags : uint16_t {
    ST_MPU_OK      = 1 << 0,
    ST_MAG_OK      = 1 << 1,
    ST_BARO_OK     = 1 << 2,
    ST_CALIBRATING = 1 << 3,
    ST_GYRO_CAL    = 1 << 4,   // giroscópio já calibrado
    ST_MAG_CAL     = 1 << 5,   // magnetômetro já calibrado
};

// ---------- Telemetria A -> B ----------
struct __attribute__((packed)) TelemetryPacket {
    MsgHeader hdr;
    uint32_t  t_ms;          // millis() no A
    // Atitude (graus)
    float     roll;
    float     pitch;
    float     heading;       // 0..360, proa magnética compensada
    // Taxas angulares (graus/s) — para indicador de curva
    float     gx, gy, gz;
    // Ambiente
    float     altitude_m;    // relativa ao ponto de referência (zerada no boot)
    float     pressure_hpa;
    float     temp_c;
    // Saúde do sistema
    uint16_t  status;        // StatusFlags
    uint16_t  loop_us;       // duração do último ciclo de fusão (medição de jitter)
};

// ---------- Comandos B -> A ----------
enum CommandId : uint8_t {
    CMD_CALIBRATE_GYRO = 1,  // manter parado durante a calibração
    CMD_CALIBRATE_MAG  = 2,  // girar o modelo em todos os eixos
    CMD_ZERO_ATTITUDE  = 3,  // posição atual vira roll = pitch = 0
    CMD_ZERO_ALTITUDE  = 4,
    CMD_SET_RATE_HZ    = 5,  // arg = nova taxa de envio
};

struct __attribute__((packed)) CommandPacket {
    MsgHeader hdr;
    uint8_t   cmd;           // CommandId
    int32_t   arg;
};

struct __attribute__((packed)) AckPacket {
    MsgHeader hdr;
    uint8_t   cmd;           // comando confirmado
    uint8_t   ok;            // 1 = sucesso
};

// ESP-NOW aceita no máximo 250 bytes de payload
static_assert(sizeof(TelemetryPacket) <= 250, "TelemetryPacket excede limite do ESP-NOW");
static_assert(sizeof(CommandPacket)   <= 250, "CommandPacket excede limite do ESP-NOW");
