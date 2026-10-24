// web_json.h — mensagens JSON do WebSocket do ESP B (docs/web.md, passo 3; contrato em
// docs/protocolo-websocket.md). Lógica pura (sem Arduino.h), testável com pio test -e native (ArduinoJson 7).
//   B → clientes: "hello" (na conexão), "tel" (telemetria a WS_RATE_HZ) e "cmd" (andamento de um comando);
//   clientes → B: {"cmd": "...", "arg": n, "pin": "..."}. Todos veem; só quem envia o PIN certo comanda.
// Um valor não finito vai como null. A validade dos dados segue no "valid" e no "cur" (há dado atual, a mesma
// regra do alarme, do OLED e do TFT): a página mostra "---" com a flag desligada ou sem dado atual.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "alarm_logic.h"
#include "command_link.h"
#include "telemetry.h"

struct WebTelemetry {
    bool            everLinked, linkOk, dataLate;   // da linkSnapshot
    uint32_t        ageMs, delayMs;                 // idade desde a chegada; atraso estimado na chegada
    TelemetryPacket pkt;                            // último pacote aceito (vale com everLinked)
    bool            haveReport;                     // há um resumo de 1 s do link
    uint32_t        rxHz;                           // pacotes na última janela de 1 s
    uint32_t        lostTotal, timeouts;
    float           lossPct;
    AlarmKind       alarm;
    bool            alarmAcked;
    float           declinationDeg;
};

// Telemetria ("tel"). Devolve o tamanho escrito (0 se não coube)
size_t webBuildTelemetry(const WebTelemetry &t, char *out, size_t n);
// Boas-vindas ("hello"): declinação, taxas aceitas e se os comandos exigem PIN
size_t webBuildHello(float declinationDeg, bool pinRequired, char *out, size_t n);
// Andamento de um comando ("cmd"), para todos os clientes
size_t webBuildCmdEvent(uint8_t cmd, int32_t arg, uint16_t seq, CmdState state, bool localFull, const char *origin,
                        char *out, size_t n);
// Recusa local, só para quem pediu (PIN errado, JSON inválido, comando desconhecido, fila cheia)
size_t webBuildCmdRefused(const char *cmdKey, const char *reason, char *out, size_t n);

enum class WebCmdParse : uint8_t { Ok, BadJson, UnknownCmd, BadPin, BadArg };
const char *webCmdParseText(WebCmdParse r);   // motivo, em português, para a página
// Comando recebido de um cliente. pin: o PIN do config.h (vazio = sem PIN). cmdKey recebe o nome lido (para a
// resposta), mesmo nas recusas, se houver
WebCmdParse webParseCommand(const char *json, size_t len, const char *pin, uint8_t &cmd, int32_t &arg,
                            char *cmdKey, size_t keyLen);

// Nomes do contrato JSON
const char *webCmdKey(uint8_t cmd);            // "calibrate_gyro", "calibrate_mag", "zero_attitude", ...
const char *webStateKey(CmdState s);           // "sent", "accepted", "done", "rejected", "no_response", ...
const char *webAlarmKey(AlarmKind k);          // "none", "bank_caution", "attitude", "att_invalid", ...
