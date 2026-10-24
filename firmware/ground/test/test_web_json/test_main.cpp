// Testes das mensagens JSON do WebSocket (lib/web_json).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include "web_json.h"

void setUp() {}
void tearDown() {}

static WebTelemetry base() {
    WebTelemetry t;
    memset(&t, 0, sizeof(t));
    t.everLinked = true;
    t.linkOk = true;
    t.ageMs = 9;
    t.delayMs = 3;
    t.pkt.hdr.seq = 1234;
    t.pkt.t_ms = 56789;
    t.pkt.roll = -1.5f;
    t.pkt.pitch = 0.25f;
    t.pkt.heading = 95.5f;
    t.pkt.altitude_m = 0.75f;
    t.pkt.pressure_hpa = 1009.5f;
    t.pkt.temp_c = 25.5f;
    t.pkt.status = 0x37;
    t.pkt.valid = 0x0D;
    t.haveReport = true;
    t.rxHz = 50;
    t.lostTotal = 4;
    t.lossPct = 0.125f;
    t.alarm = AlarmKind::None;
    t.declinationDeg = -20.7f;
    return t;
}

static JsonDocument parse(const char *s) {
    JsonDocument d;
    TEST_ASSERT_TRUE(deserializeJson(d, s) == DeserializationError::Ok);
    return d;
}

static void test_telemetry_fields() {
    char buf[600];
    TEST_ASSERT_TRUE(webBuildTelemetry(base(), buf, sizeof(buf)) > 0);
    JsonDocument d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("tel", d["type"]);
    TEST_ASSERT_TRUE(d["cur"].as<bool>());
    TEST_ASSERT_EQUAL_INT(1234, d["seq"].as<int>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.5f, d["roll"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 95.5f, d["hdg"].as<float>());
    TEST_ASSERT_EQUAL_INT(13, d["valid"].as<int>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -20.7f, d["decl"].as<float>());
    TEST_ASSERT_EQUAL_STRING("ok", d["link"]["state"]);
    TEST_ASSERT_EQUAL_INT(50, d["link"]["rx_hz"].as<int>());
    TEST_ASSERT_EQUAL_INT(3, d["link"]["delay_ms"].as<int>());
    TEST_ASSERT_EQUAL_STRING("none", d["alarm"]["kind"]);
}

// Valor não finito vai como null (JSON válido)
static void test_non_finite_is_null() {
    WebTelemetry t = base();
    t.pkt.altitude_m = NAN;
    t.pkt.roll = INFINITY;
    char buf[600];
    webBuildTelemetry(t, buf, sizeof(buf));
    JsonDocument d = parse(buf);
    TEST_ASSERT_TRUE(d["alt"].isNull());
    TEST_ASSERT_TRUE(d["roll"].isNull());
    TEST_ASSERT_FALSE(d["pitch"].isNull());
}

static void test_link_states() {
    char buf[600];
    WebTelemetry t = base();
    t.everLinked = false;
    t.linkOk = false;
    webBuildTelemetry(t, buf, sizeof(buf));
    JsonDocument d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("no_signal", d["link"]["state"]);
    TEST_ASSERT_FALSE(d["cur"].as<bool>());
    TEST_ASSERT_TRUE(d["roll"].isNull());   // sem pacote: nem o campo vai
    t = base();
    t.linkOk = false;
    webBuildTelemetry(t, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("lost", parse(buf)["link"]["state"]);
    t = base();
    t.dataLate = true;
    t.alarm = AlarmKind::DataLate;
    webBuildTelemetry(t, buf, sizeof(buf));
    d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("late", d["link"]["state"]);
    TEST_ASSERT_FALSE(d["cur"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("data_late", d["alarm"]["kind"]);
}

static void test_buffer_too_small() {
    char buf[20];
    TEST_ASSERT_EQUAL_size_t(0, webBuildTelemetry(base(), buf, sizeof(buf)));
}

static void test_hello_and_events() {
    char buf[300];
    webBuildHello(-20.7f, true, buf, sizeof(buf));
    JsonDocument d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("hello", d["type"]);
    TEST_ASSERT_TRUE(d["pin"].as<bool>());
    TEST_ASSERT_EQUAL_INT(4, d["rates"].size());
    webBuildCmdEvent(CMD_SET_RATE_HZ, 25, 777, CmdState::Accepted, false, "página", buf, sizeof(buf));
    d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("cmd", d["type"]);
    TEST_ASSERT_EQUAL_STRING("set_rate", d["cmd"]);
    TEST_ASSERT_EQUAL_INT(25, d["arg"].as<int>());
    TEST_ASSERT_EQUAL_STRING("accepted", d["state"]);
    webBuildCmdEvent(CMD_ZERO_ATTITUDE, 0, 1, CmdState::Rejected, true, "página", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("busy", parse(buf)["state"]);
    webBuildCmdRefused("zero_attitude", "PIN incorreto", buf, sizeof(buf));
    d = parse(buf);
    TEST_ASSERT_EQUAL_STRING("refused", d["state"]);
    TEST_ASSERT_EQUAL_STRING("PIN incorreto", d["reason"]);
}

static void test_parse_commands() {
    uint8_t cmd;
    int32_t arg;
    char key[24];
    const char *ok = "{\"cmd\":\"zero_attitude\",\"pin\":\"4321\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::Ok, webParseCommand(ok, strlen(ok), "4321", cmd, arg, key, sizeof(key)));
    TEST_ASSERT_EQUAL_UINT8(CMD_ZERO_ATTITUDE, cmd);
    TEST_ASSERT_EQUAL_STRING("zero_attitude", key);
    const char *rate = "{\"cmd\":\"set_rate\",\"arg\":25,\"pin\":\"4321\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::Ok, webParseCommand(rate, strlen(rate), "4321", cmd, arg, key, sizeof(key)));
    TEST_ASSERT_EQUAL_UINT8(CMD_SET_RATE_HZ, cmd);
    TEST_ASSERT_EQUAL_INT32(25, arg);
}

static void test_parse_refusals() {
    uint8_t cmd;
    int32_t arg;
    char key[24];
    const char *nopin = "{\"cmd\":\"calibrate_gyro\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::BadPin, webParseCommand(nopin, strlen(nopin), "4321", cmd, arg, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("calibrate_gyro", key);
    const char *wrong = "{\"cmd\":\"calibrate_gyro\",\"pin\":\"0000\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::BadPin, webParseCommand(wrong, strlen(wrong), "4321", cmd, arg, key, sizeof(key)));
    const char *unk = "{\"cmd\":\"reboot\",\"pin\":\"4321\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::UnknownCmd, webParseCommand(unk, strlen(unk), "4321", cmd, arg, key, sizeof(key)));
    const char *bad = "{cmd:";
    TEST_ASSERT_EQUAL(WebCmdParse::BadJson, webParseCommand(bad, strlen(bad), "4321", cmd, arg, key, sizeof(key)));
    const char *arr = "[1,2]";
    TEST_ASSERT_EQUAL(WebCmdParse::BadJson, webParseCommand(arr, strlen(arr), "4321", cmd, arg, key, sizeof(key)));
    const char *noarg = "{\"cmd\":\"set_rate\",\"pin\":\"4321\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::BadArg, webParseCommand(noarg, strlen(noarg), "4321", cmd, arg, key, sizeof(key)));
    const char *strarg = "{\"cmd\":\"set_rate\",\"arg\":\"25\",\"pin\":\"4321\"}";
    TEST_ASSERT_EQUAL(WebCmdParse::BadArg, webParseCommand(strarg, strlen(strarg), "4321", cmd, arg, key, sizeof(key)));
    // Sem PIN configurado, qualquer um comanda
    TEST_ASSERT_EQUAL(WebCmdParse::Ok, webParseCommand(nopin, strlen(nopin), "", cmd, arg, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("PIN incorreto", webCmdParseText(WebCmdParse::BadPin));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_fields);
    RUN_TEST(test_non_finite_is_null);
    RUN_TEST(test_link_states);
    RUN_TEST(test_buffer_too_small);
    RUN_TEST(test_hello_and_events);
    RUN_TEST(test_parse_commands);
    RUN_TEST(test_parse_refusals);
    return UNITY_END();
}
