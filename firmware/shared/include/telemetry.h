// telemetry.h — contrato de dados entre ESP A (avião) e ESP B (solo)
// Compartilhado pelos dois projetos via build_flags = -I../shared/include
// Qualquer mudança aqui exige recompilar e regravar OS DOIS firmwares, incrementar TELEMETRY_VERSION
// e espelhar no docs/protocolo-websocket.md.
//
// Layout: structs "packed" (nenhum compilador insere enchimento), com campos de reserva explícitos
// para que cada campo fique num offset múltiplo do próprio tamanho. Os static_assert no fim conferem.
//
// Recepção no B: SEMPRE copiar o buffer do ESP-NOW com memcpy para um struct local, antes de
// enfileirar. Nunca fazer cast do ponteiro recebido: o buffer não tem alinhamento garantido, e
// acessar um float desalinhado derruba o Xtensa (LoadStoreAlignment).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_MAGIC    0xA7   // identifica pacotes do projeto
#define TELEMETRY_VERSION  4      // incrementar ao mudar qualquer struct (ou o significado de um campo)
// v2 (15/10/2026): cabeçalho de 8 bytes (alinhamento) e campo valid (validade dos dados)
// v3 (15/10/2026): ACK com result e cmd_seq; VAL_HDG exige o magnetômetro calibrado
// v4 (21/10/2026): loop_us passa a ser a latência do dado (pulso do INT → fim da fusão), sem mudar o layout

// ---------- Tipos de mensagem ----------
enum MsgType : uint8_t {
    MSG_TELEMETRY = 1,   // A -> B, periódica (~50 Hz)
    MSG_COMMAND   = 2,   // B -> A, sob demanda
    MSG_ACK       = 3,   // A -> B, confirmação de comando
};

// ---------- Cabeçalho comum (8 bytes) ----------
struct __attribute__((packed)) MsgHeader {
    uint8_t  magic;      // TELEMETRY_MAGIC
    uint8_t  version;    // TELEMETRY_VERSION; o B descarta (e conta) outras versões
    uint8_t  type;       // MsgType
    uint8_t  reserved0;  // 0
    uint16_t seq;        // contador; B usa para detectar pacotes perdidos
    uint16_t reserved1;  // 0
};

// ---------- Saúde dos sensores e calibração (campo status) ----------
// Diz se o sensor responde e se há calibração, não se o dado vale agora: para isso, ver ValidFlags.
enum StatusFlags : uint16_t {
    ST_MPU_OK      = 1 << 0,
    ST_MAG_OK      = 1 << 1,
    ST_BARO_OK     = 1 << 2,
    ST_CALIBRATING = 1 << 3,
    ST_GYRO_CAL    = 1 << 4,   // giroscópio já calibrado
    ST_MAG_CAL     = 1 << 5,   // magnetômetro já calibrado
};

// ---------- Validade dos dados (campo valid) ----------
// Com a flag desligada, o B mostra o dado como indisponível (horizonte riscado, "---"), em vez de
// congelar o último valor. A idade de cada sensor entra na flag: um sensor parado apaga o dado sozinho.
// Regras em firmware/aircraft/lib/telemetry_flags (testadas no native).
enum ValidFlags : uint16_t {
    VAL_ATT           = 1 << 0,   // atitude: filtro inicializado e última amostra do MPU recente
    VAL_ATT_DEGRADED  = 1 << 1,   // com VAL_ATT: giroscópio saturou, correção pelo acelerômetro se recuperando
    VAL_ALT           = 1 << 2,   // altitude: barômetro ok, zero válido, leitura recente e valor finito
    VAL_HDG           = 1 << 3,   // proa: filtro da proa inicializado, VAL_ATT e magnetômetro calibrado
                                  // (sem calibração a proa pode errar dezenas de graus)
    VAL_HDG_GYRO_ONLY = 1 << 4,   // com VAL_HDG: correção pelo magnetômetro suspensa (atitude degradada, já
                                  // no mesmo pacote de VAL_ATT_DEGRADED; distúrbio; inclinação alta) ou
                                  // magnetômetro sem leitura recente; a proa segue só pelo giroscópio e pode derivar
};

// ---------- Telemetria A -> B (56 bytes) ----------
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
    uint16_t  valid;         // ValidFlags
    uint16_t  loop_us;       // latência da última amostra fundida: pulso do INT → fim da fusão (µs, satura
                             // em 65535). Medição de jitter e de latência (semana 6)
    uint16_t  reserved2;     // 0
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
    uint8_t   reserved[3];   // 0
    int32_t   arg;
};

// Resultado de um comando. Os estados ficam reservados no contrato desde a v3; a lógica (aceito ao
// receber, concluído ao terminar, recusado com o motivo na serial) entra com os comandos (semana 5).
enum AckResult : uint8_t {
    ACK_REJECTED = 0,   // não executado (inválido, outro procedimento em andamento, sensor indisponível)
    ACK_ACCEPTED = 1,   // recebido e iniciado; um ACK_DONE vem depois, nos comandos demorados
    ACK_DONE     = 2,   // concluído com sucesso
};

struct __attribute__((packed)) AckPacket {
    MsgHeader hdr;           // hdr.seq: contador de mensagens do A
    uint8_t   cmd;           // comando confirmado (CommandId)
    uint8_t   result;        // AckResult
    uint16_t  cmd_seq;       // hdr.seq do CommandPacket confirmado: o B casa o ACK com o comando e
                             // descarta ACK duplicado ou atrasado
};

// ---------- Conferência do layout ----------
// ESP-NOW aceita no máximo 250 bytes de payload
static_assert(sizeof(TelemetryPacket) <= 250, "TelemetryPacket excede limite do ESP-NOW");
static_assert(sizeof(CommandPacket)   <= 250, "CommandPacket excede limite do ESP-NOW");

static_assert(sizeof(MsgHeader) == 8, "MsgHeader deve ter 8 bytes");
static_assert(offsetof(MsgHeader, seq) == 4, "seq fora do lugar");

static_assert(sizeof(TelemetryPacket) == 56, "TelemetryPacket deve ter 56 bytes");
static_assert(offsetof(TelemetryPacket, t_ms) == 8, "t_ms fora do lugar");
static_assert(offsetof(TelemetryPacket, roll) == 12, "roll fora do lugar");
static_assert(offsetof(TelemetryPacket, pitch) == 16, "pitch fora do lugar");
static_assert(offsetof(TelemetryPacket, heading) == 20, "heading fora do lugar");
static_assert(offsetof(TelemetryPacket, gx) == 24, "gx fora do lugar");
static_assert(offsetof(TelemetryPacket, gy) == 28, "gy fora do lugar");
static_assert(offsetof(TelemetryPacket, gz) == 32, "gz fora do lugar");
static_assert(offsetof(TelemetryPacket, altitude_m) == 36, "altitude_m fora do lugar");
static_assert(offsetof(TelemetryPacket, pressure_hpa) == 40, "pressure_hpa fora do lugar");
static_assert(offsetof(TelemetryPacket, temp_c) == 44, "temp_c fora do lugar");
static_assert(offsetof(TelemetryPacket, status) == 48, "status fora do lugar");
static_assert(offsetof(TelemetryPacket, valid) == 50, "valid fora do lugar");
static_assert(offsetof(TelemetryPacket, loop_us) == 52, "loop_us fora do lugar");

static_assert(sizeof(CommandPacket) == 16, "CommandPacket deve ter 16 bytes");
static_assert(offsetof(CommandPacket, cmd) == 8, "cmd fora do lugar");
static_assert(offsetof(CommandPacket, arg) == 12, "arg fora do lugar");

static_assert(sizeof(AckPacket) == 12, "AckPacket deve ter 12 bytes");
static_assert(offsetof(AckPacket, cmd) == 8, "cmd do ACK fora do lugar");
static_assert(offsetof(AckPacket, result) == 9, "result do ACK fora do lugar");
static_assert(offsetof(AckPacket, cmd_seq) == 10, "cmd_seq do ACK fora do lugar");
