// web_json.cpp — ver web_json.h
#include "web_json.h"
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

const char *webCmdKey(uint8_t cmd) {
    switch (cmd) {
    case CMD_CALIBRATE_GYRO: return "calibrate_gyro";
    case CMD_CALIBRATE_MAG:  return "calibrate_mag";
    case CMD_ZERO_ATTITUDE:  return "zero_attitude";
    case CMD_ZERO_ALTITUDE:  return "zero_altitude";
    case CMD_SET_RATE_HZ:    return "set_rate";
    }
    return "unknown";
}

const char *webStateKey(CmdState s) {
    switch (s) {
    case CmdState::Free:         return "free";
    case CmdState::Sending:      return "sent";
    case CmdState::Accepted:     return "accepted";
    case CmdState::Done:         return "done";
    case CmdState::Rejected:     return "rejected";
    case CmdState::NoResponse:   return "no_response";
    case CmdState::NoCompletion: return "no_completion";
    }
    return "?";
}

const char *webAlarmKey(AlarmKind k) {
    switch (k) {
    case AlarmKind::None:            return "none";
    case AlarmKind::BankCaution:     return "bank_caution";
    case AlarmKind::AttitudeWarning: return "attitude";
    case AlarmKind::AttInvalid:      return "att_invalid";
    case AlarmKind::DataLate:        return "data_late";
    case AlarmKind::LinkLost:        return "link_lost";
    }
    return "?";
}

const char *webCmdParseText(WebCmdParse r) {
    switch (r) {
    case WebCmdParse::Ok:         return "ok";
    case WebCmdParse::BadJson:    return "mensagem inválida";
    case WebCmdParse::UnknownCmd: return "comando desconhecido";
    case WebCmdParse::BadPin:     return "PIN incorreto";
    case WebCmdParse::BadArg:     return "argumento inválido";
    }
    return "?";
}

// Número finito, ou null. Recebe o documento e o nome do campo (o proxy doc["x"] não pode ser copiado no
// compilador do ESP32)
static void setNum(JsonDocument &doc, const char *key, float x) {
    if (isfinite(x)) doc[key] = x;
    else doc[key] = nullptr;
}

static size_t finish(JsonDocument &doc, char *out, size_t n) {
    const size_t need = measureJson(doc);
    if (need + 1 > n) return 0;
    return serializeJson(doc, out, n);
}

size_t webBuildTelemetry(const WebTelemetry &t, char *out, size_t n) {
    JsonDocument doc;
    doc["type"] = "tel";
    const bool cur = t.everLinked && t.linkOk && !t.dataLate;
    doc["cur"] = cur;
    if (t.everLinked) {
        const TelemetryPacket &p = t.pkt;
        doc["t"] = p.t_ms;
        doc["seq"] = p.hdr.seq;
        setNum(doc, "roll", p.roll);
        setNum(doc, "pitch", p.pitch);
        setNum(doc, "hdg", p.heading);
        setNum(doc, "gx", p.gx);
        setNum(doc, "gy", p.gy);
        setNum(doc, "gz", p.gz);
        setNum(doc, "alt", p.altitude_m);
        setNum(doc, "hpa", p.pressure_hpa);
        setNum(doc, "temp", p.temp_c);
        doc["status"] = p.status;
        doc["valid"] = p.valid;
    }
    doc["decl"] = t.declinationDeg;
    JsonObject link = doc["link"].to<JsonObject>();
    link["state"] = !t.everLinked ? "no_signal" : !t.linkOk ? "lost" : t.dataLate ? "late" : "ok";
    if (t.everLinked) {
        link["age_ms"] = t.ageMs;
        link["delay_ms"] = t.delayMs;
    }
    if (t.haveReport) {
        link["rx_hz"] = t.rxHz;
        link["lost"] = t.lostTotal;
        link["loss_pct"] = t.lossPct;
        link["drops"] = t.timeouts;
    }
    JsonObject alarm = doc["alarm"].to<JsonObject>();
    alarm["kind"] = webAlarmKey(t.alarm);
    alarm["acked"] = t.alarmAcked;
    return finish(doc, out, n);
}

size_t webBuildHello(float declinationDeg, bool pinRequired, char *out, size_t n) {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["decl"] = declinationDeg;
    doc["pin"] = pinRequired;
    JsonArray rates = doc["rates"].to<JsonArray>();
    for (int r : {10, 20, 25, 50}) rates.add(r);
    return finish(doc, out, n);
}

size_t webBuildCmdEvent(uint8_t cmd, int32_t arg, uint16_t seq, CmdState state, bool localFull, const char *origin,
                        char *out, size_t n) {
    JsonDocument doc;
    doc["type"] = "cmd";
    doc["cmd"] = webCmdKey(cmd);
    if (cmd == CMD_SET_RATE_HZ) doc["arg"] = arg;
    doc["seq"] = seq;
    doc["state"] = localFull ? "busy" : webStateKey(state);
    doc["origin"] = origin;
    return finish(doc, out, n);
}

size_t webBuildCmdRefused(const char *cmdKey, const char *reason, char *out, size_t n) {
    JsonDocument doc;
    doc["type"] = "cmd";
    doc["cmd"] = cmdKey != nullptr && cmdKey[0] ? cmdKey : "unknown";
    doc["state"] = "refused";
    doc["reason"] = reason;
    return finish(doc, out, n);
}

WebCmdParse webParseCommand(const char *json, size_t len, const char *pin, uint8_t &cmd, int32_t &arg,
                            char *cmdKey, size_t keyLen) {
    if (keyLen > 0) cmdKey[0] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok || !doc.is<JsonObject>()) return WebCmdParse::BadJson;
    const char *name = doc["cmd"] | "";
    if (keyLen > 0) {
        strncpy(cmdKey, name, keyLen - 1);
        cmdKey[keyLen - 1] = '\0';
    }
    static const uint8_t ALL[] = {CMD_CALIBRATE_GYRO, CMD_CALIBRATE_MAG, CMD_ZERO_ATTITUDE, CMD_ZERO_ALTITUDE,
                                  CMD_SET_RATE_HZ};
    cmd = 0;
    for (uint8_t c : ALL)
        if (strcmp(name, webCmdKey(c)) == 0) cmd = c;
    if (cmd == 0) return WebCmdParse::UnknownCmd;
    // O PIN é conferido antes do argumento: sem ele, a mensagem não diz nada ao cliente sobre o comando
    if (pin != nullptr && pin[0] != '\0') {
        const char *got = doc["pin"] | "";
        if (strcmp(got, pin) != 0) return WebCmdParse::BadPin;
    }
    arg = 0;
    if (cmd == CMD_SET_RATE_HZ) {
        if (!doc["arg"].is<int>()) return WebCmdParse::BadArg;   // o A confere a faixa (10, 20, 25, 50)
        arg = doc["arg"].as<int32_t>();
    }
    return WebCmdParse::Ok;
}
