// web_task.cpp — ver web_task.h
#include "web_task.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include "alarm_task.h"
#include "command_task.h"
#include "config.h"
#include "http_static.h"
#include "link_task.h"
#include "web_json.h"
#include "ws_outbox.h"

static WiFiServer s_http(HTTP_PORT);
// WebSocketsServer com acesso ao socket de cada cliente. A biblioteca escreve esperando até WEBSOCKETS_TCP_TIMEOUT
// (5 s) com o buffer de envio cheio, por cima das esperas do WiFiClient::write: um celular que parou de ler (tela
// apagada, fora do alcance) travaria a task inteira. Por isso a task só envia a quem tem espaço e derruba quem
// fica WS_STALL_MS sem espaço
class WsServer : public WebSocketsServer {
public:
    using WebSocketsServer::WebSocketsServer;
    // Buffer de envio do TCP acima da marca baixa do lwip (TCP_SNDLOWAT, ~2,8 kB): uma mensagem menor que isso
    // sai inteira sem esperar
    bool canSend(uint8_t num) {
        WiFiClient *t = _clients[num].tcp;
        if (t == nullptr || t->fd() < 0) return false;
        const int fd = t->fd();
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv = {0, 0};
        return select(fd + 1, nullptr, &set, nullptr, &tv) > 0 && FD_ISSET(fd, &set);
    }
    // Handshake concluído: antes disso o sendTXT recusa
    bool ready(uint8_t num) { return clientIsConnected(num) && _clients[num].status == WSC_CONNECTED; }
    // Derruba sem o quadro de fechamento (que esperaria pelo buffer cheio); o loop() da biblioteca libera a vaga
    void drop(uint8_t num) {
        if (_clients[num].tcp != nullptr) _clients[num].tcp->stop();
    }
};
static_assert(WEB_MSG_BYTES + 16 < TCP_SNDLOWAT, "mensagem maior que a marca baixa do envio: canSend não garante");

static_assert(WEBSOCKETS_SERVER_CLIENT_MAX == AP_MAX_CLIENTS,
              "WEBSOCKETS_SERVER_CLIENT_MAX (platformio.ini) diferente do AP_MAX_CLIENTS (config.h)");
static_assert(2 + AP_MAX_CLIENTS + HTTP_MAX_CONNS <= CONFIG_LWIP_MAX_SOCKETS - 2,
              "sockets demais para o SDK: folga de 2 para recarregamentos");

static WsServer s_ws(WS_PORT);
static uint32_t s_wsOkMs[WEBSOCKETS_SERVER_CLIENT_MAX];   // último envio aceito de cada cliente

// Mensagem que não pode se perder, guardada como dados (o JSON é montado na hora do envio)
struct WsOut {
    enum class Kind : uint8_t { Hello, Cmd, Refused } kind;
    CmdEventMsg ce;          // Cmd
    char key[24];            // Refused: nome do comando
    const char *reason;      // Refused: texto fixo (webCmdParseText ou literal)
};
static_assert(WS_OUTBOX_LEN >= WS_RECENT_CMDS + 2,
              "a fila precisa caber o hello, os comandos recentes e uma recusa (ressincronização)");
static OutRing<WsOut, WS_OUTBOX_LEN> s_out[WEBSOCKETS_SERVER_CLIENT_MAX];
static RecentBySeq<CmdEventMsg, WS_RECENT_CMDS> s_recent;   // último estado dos comandos recentes
static char s_tel[WEB_MSG_BYTES];                           // telemetria (s_msg: as mensagens da fila)
static QueueHandle_t s_status = nullptr;
static WebStatus s_st = {};
static char s_msg[WEB_MSG_BYTES];

// ---------- HTTP: arquivos do LittleFS, várias conexões sem travar ----------
// Cada conexão tem seu estado e avança um pouco a cada volta da task: lê a linha de requisição, responde com o
// cabeçalho e manda o arquivo em pedaços de HTTP_CHUNK_BYTES. Ociosa ou parada por HTTP_IDLE_MS: fechada.

enum class ConnState : uint8_t { Free, Reading, Sending };

struct HttpConn {
    ConnState state;
    WiFiClient client;
    uint32_t lastMs;            // último progresso (dado recebido ou enviado)
    char req[HTTP_REQ_BYTES];
    size_t reqLen;
    char head[256];             // cabeçalho da resposta
    size_t headLen, headSent;
    char text[128];             // corpo das respostas de erro (sem arquivo)
    size_t textLen, textSent;
    File file;                  // corpo das respostas 200
};

static HttpConn s_conns[HTTP_MAX_CONNS];
static uint8_t s_chunk[HTTP_CHUNK_BYTES];

static void connClose(HttpConn &c) {
    if (c.file) c.file.close();
    c.client.stop();
    c.state = ConnState::Free;
}

// Descarta o resto da requisição (cabeçalhos): fechar com dados não lidos faz o TCP mandar RST, e o navegador
// pode perder o fim da resposta
static void connDrain(HttpConn &c) {
    while (c.client.available() > 0) c.client.read(s_chunk, sizeof(s_chunk));
}

static void respondText(HttpConn &c, int code, const char *body) {
    c.textLen = strlcpy(c.text, body, sizeof(c.text));
    if (c.textLen >= sizeof(c.text)) c.textLen = sizeof(c.text) - 1;
    c.textSent = 0;
    c.headLen = httpBuildHeader(code, "text/plain; charset=utf-8", c.textLen, c.head, sizeof(c.head));
    c.headSent = 0;
    c.state = ConnState::Sending;
}

static void respondRequest(HttpConn &c, HttpParse r, const char *path) {
    s_st.httpReqs++;
    if (r == HttpParse::NotGet) return respondText(c, 405, "Somente GET.");
    if (r == HttpParse::TooLong) return respondText(c, 414, "Requisicao longa demais.");
    if (r != HttpParse::Ok) return respondText(c, 400, "Requisicao invalida.");
    if (!s_st.fsOk)
        return respondText(c, 503, "Pagina nao gravada no ESP B: rode 'pio run -e ground -t uploadfs' em firmware/ground.");
    c.file = LittleFS.open(path, "r");
    if (!c.file || c.file.isDirectory()) {
        if (c.file) c.file.close();
        char body[HTTP_PATH_MAX + 24];
        snprintf(body, sizeof(body), "Nao encontrado: %s", path);
        return respondText(c, 404, body);
    }
    c.textLen = c.textSent = 0;
    c.headLen = httpBuildHeader(200, httpContentType(path), c.file.size(), c.head, sizeof(c.head));
    c.headSent = 0;
    c.state = ConnState::Sending;
}

// Escreve até n bytes sem esperar; devolve quantos saíram (0 com o buffer de envio do TCP cheio). Não usa o
// WiFiClient::write: ele espera até 1 s por tentativa, com até 10 tentativas que recomeçam a cada byte enviado,
// e um cliente lento travaria a task inteira (HTTP, WebSocket e eventos de comando)
static size_t connWrite(HttpConn &c, const uint8_t *p, size_t n) {
    const int w = send(c.client.fd(), p, n, MSG_DONTWAIT);
    if (w > 0) {
        c.lastMs = millis();
        return (size_t)w;
    }
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) connClose(c);   // conexão caiu
    return 0;
}

static void connStep(HttpConn &c, uint32_t now) {
    if (!c.client.connected() && c.client.available() <= 0) {
        connClose(c);
        return;
    }
    if (now - c.lastMs > HTTP_IDLE_MS) {
        s_st.httpIdle++;
        connClose(c);
        return;
    }
    if (c.state == ConnState::Reading) {
        int avail = c.client.available();
        if (avail <= 0) return;
        const size_t room = sizeof(c.req) - c.reqLen;
        if (room > 0) {
            const int got = c.client.read((uint8_t *)c.req + c.reqLen, min((size_t)avail, room));
            if (got > 0) {
                c.reqLen += (size_t)got;
                c.lastMs = now;
            }
        }
        char path[HTTP_PATH_MAX];
        const HttpParse r = httpParseRequest(c.req, c.reqLen, sizeof(c.req), path, sizeof(path));
        if (r == HttpParse::NeedMore) return;
        respondRequest(c, r, path);
        c.lastMs = now;
    }
    // Sending: cabeçalho, depois o corpo, um pedaço por volta
    connDrain(c);
    if (c.headSent < c.headLen) {
        c.headSent += connWrite(c, (const uint8_t *)c.head + c.headSent, c.headLen - c.headSent);
        if (c.headSent < c.headLen) return;
    }
    if (c.file) {
        const size_t pos = c.file.position();
        const size_t n = c.file.read(s_chunk, sizeof(s_chunk));
        if (n == 0) {
            connClose(c);   // arquivo inteiro enviado
            return;
        }
        const size_t w = connWrite(c, s_chunk, n);
        if (c.state == ConnState::Free) return;   // caiu no envio
        if (w < n) c.file.seek(pos + w);   // o resto vai na próxima volta
        return;
    }
    if (c.textSent < c.textLen) {
        c.textSent += connWrite(c, (const uint8_t *)c.text + c.textSent, c.textLen - c.textSent);
        if (c.textSent < c.textLen) return;
    }
    connClose(c);
}

// Nova conexão: uma vaga livre; sem vaga, a que espera requisição há mais tempo dá lugar (os navegadores abrem
// conexões antecipadas que talvez nunca usem)
static void httpAccept(uint32_t now) {
    WiFiClient nc = s_http.available();
    if (!nc) return;
    HttpConn *slot = nullptr;
    for (HttpConn &c : s_conns)
        if (c.state == ConnState::Free) {
            slot = &c;
            break;
        }
    if (slot == nullptr) {
        for (HttpConn &c : s_conns)
            if (c.state == ConnState::Reading && (slot == nullptr || now - c.lastMs > now - slot->lastMs)) slot = &c;
        if (slot == nullptr) {
            s_st.httpRejected++;
            nc.stop();
            return;
        }
        s_st.httpEvicted++;
        connClose(*slot);
    }
    nc.setNoDelay(true);
    slot->client = nc;
    slot->state = ConnState::Reading;
    slot->reqLen = 0;
    slot->lastMs = now;
}

static void httpLoop() {
    const uint32_t now = millis();
    httpAccept(now);
    uint8_t open = 0;
    for (HttpConn &c : s_conns) {
        if (c.state != ConnState::Free) connStep(c, now);
        if (c.state != ConnState::Free) open++;
    }
    s_st.httpOpen = open;
}

// ---------- WebSocket ----------

// Refaz a fila do cliente: hello e o último estado dos comandos recentes (do mais antigo ao mais novo). Na
// conexão e quando a fila enche: o RecentBySeq é a fonte da verdade do andamento, então nada de um comando se
// perde (a fila cheia descartaria a mais antiga, que podia ser um "done")
static void wsResync(uint8_t num) {
    s_out[num].clear();
    WsOut o = {};
    o.kind = WsOut::Kind::Hello;
    s_out[num].push(o);
    o.kind = WsOut::Kind::Cmd;
    for (size_t i = 0; i < s_recent.size(); i++) {
        o.ce = s_recent.get(i);
        s_out[num].push(o);
    }
}

// O andamento de um comando já na fila substitui o anterior no lugar (só o estado mais recente importa)
static void wsPush(uint8_t num, const WsOut &o) {
    if (o.kind == WsOut::Kind::Cmd &&
        s_out[num].replaceFirst(
            [&o](const WsOut &x) { return x.kind == WsOut::Kind::Cmd && x.ce.ev.seq == o.ce.ev.seq; }, o))
        return;
    if (s_out[num].push(o)) return;
    // Cheia: o s_recent já tem o evento novo (atualizado antes do envio), e uma recusa entra depois
    s_st.wsResyncs++;
    wsResync(num);
    if (o.kind == WsOut::Kind::Refused) s_out[num].push(o);
}

static size_t buildOut(const WsOut &o) {
    switch (o.kind) {
    case WsOut::Kind::Hello:
        return webBuildHello(MAG_DECLINATION_DEG, WEB_CMD_PIN[0] != '\0', s_msg, sizeof(s_msg));
    case WsOut::Kind::Cmd:
        return webBuildCmdEvent(o.ce.ev.cmd, o.ce.ev.arg, o.ce.ev.seq, o.ce.ev.state, o.ce.localFull,
                                cmdOriginText(o.ce.ev.origin), s_msg, sizeof(s_msg));
    case WsOut::Kind::Refused:
        return webBuildCmdRefused(o.key, o.reason, s_msg, sizeof(s_msg));
    }
    return 0;
}

// Cliente com algo a enviar e sem espaço: WS_STALL_MS assim, ele cai (a página reconecta sozinha e recebe os
// comandos recentes na conexão)
static void wsStalled(uint8_t num, uint32_t now) {
    if (now - s_wsOkMs[num] <= WS_STALL_MS) return;
    s_ws.drop(num);
    s_out[num].clear();
    s_st.wsDropped++;
}

// Envia sem esperar o que está na fila do cliente, na ordem; o que não coube fica para a próxima volta.
// true com a fila vazia
static bool wsFlush(uint8_t num, uint32_t now) {
    WsOut o;
    while (s_out[num].peek(o)) {
        if (!s_ws.canSend(num)) {
            wsStalled(num, now);
            return false;
        }
        const size_t n = buildOut(o);
        if (n > 0 && !s_ws.sendTXT(num, s_msg, n)) {
            s_st.wsSendFail++;
            wsStalled(num, now);
            return false;
        }
        s_wsOkMs[num] = now;
        s_out[num].pop();
    }
    return true;
}

// Telemetria: substituível pela próxima, então só sai para quem tem a fila vazia e espaço no envio
static void wsSendTelemetry(uint8_t num, size_t n, uint32_t now) {
    if (!s_out[num].empty() || !s_ws.canSend(num)) {
        s_st.wsSkipped++;
        wsStalled(num, now);
        return;
    }
    if (!s_ws.sendTXT(num, s_tel, n)) {
        s_st.wsSendFail++;
        wsStalled(num, now);
        return;
    }
    s_wsOkMs[num] = now;
}

static void sendRefused(uint8_t num, const char *cmdKey, const char *reason) {
    WsOut o = {};
    o.kind = WsOut::Kind::Refused;
    snprintf(o.key, sizeof(o.key), "%s", cmdKey);
    o.reason = reason;
    wsPush(num, o);
    s_st.cmdsRefused++;
}

static void onWs(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED: {
        s_wsOkMs[num] = millis();
        wsResync(num);   // hello e os comandos recentes
        break;
    }
    case WStype_DISCONNECTED:
        s_out[num].clear();
        break;
    case WStype_TEXT: {
        uint8_t cmd;
        int32_t arg;
        char key[24];
        const WebCmdParse r = webParseCommand((const char *)payload, len, WEB_CMD_PIN, cmd, arg, key, sizeof(key));
        if (r != WebCmdParse::Ok) {
            sendRefused(num, key, webCmdParseText(r));
            return;
        }
        if (!commandSubmit(cmd, arg, CMD_FROM_WEB)) {
            sendRefused(num, key, "fila de comandos cheia");
            return;
        }
        s_st.cmdsOk++;   // o andamento chega pelos eventos da command_task, para todos os clientes
        break;
    }
    default:
        break;
    }
}

// Telemetria a partir das mesmas cópias do OLED e do TFT
static size_t buildTelemetry() {
    WebTelemetry t = {};
    const LinkSnapshot snap = linkSnapshot((uint64_t)esp_timer_get_time());
    t.everLinked = snap.everLinked;
    t.linkOk = snap.linkOk;
    t.dataLate = snap.dataLate;
    t.ageMs = snap.ageMs;
    t.delayMs = snap.item.delayMs;
    t.pkt = snap.item.pkt;
    LinkReport r;
    if (linkLatestReport(r)) {
        t.haveReport = true;
        t.rxHz = r.windowMs > 0 ? (r.win.counts.received * 1000 + r.windowMs / 2) / r.windowMs : 0;
        t.lostTotal = r.totals.lost;
        t.timeouts = r.totals.timeouts;
        const uint32_t expected = r.totals.received + r.totals.lost;
        t.lossPct = expected > 0 ? 100.0f * r.totals.lost / expected : 0.0f;
    }
    AlarmStatus al;
    if (alarmCurrent(al)) {
        t.alarm = al.kind;
        t.alarmAcked = al.acked;
    }
    t.declinationDeg = MAG_DECLINATION_DEG;
    return webBuildTelemetry(t, s_tel, sizeof(s_tel));
}

static void webTask(void *) {
    // Sem formatar: um LittleFS ausente ou corrompido só desliga a página (a mensagem explica o uploadfs)
    s_st.fsOk = LittleFS.begin(false) && LittleFS.exists("/index.html");
    s_http.begin();   // todas as rotas: arquivos do LittleFS ("/" = index.html)
    s_http.setNoDelay(true);
    s_ws.begin();
    s_ws.onEvent(onWs);
    const uint32_t telPeriodMs = 1000 / WS_RATE_HZ;
    uint32_t lastTelMs = 0;
    for (;;) {
        httpLoop();
        s_ws.loop();
        CmdEventMsg ce;
        while (commandWebEventReceive(ce)) {
            s_recent.upsert(ce.ev.seq, ce);
            WsOut o = {};
            o.kind = WsOut::Kind::Cmd;
            o.ce = ce;
            for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
                if (s_ws.ready(i)) wsPush(i, o);
        }
        const uint32_t now = millis();
        for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
            if (s_ws.ready(i)) wsFlush(i, now);
        s_st.clients = (uint8_t)s_ws.connectedClients();
        if (s_st.clients > 0 && now - lastTelMs >= telPeriodMs) {
            lastTelMs = now;
            const size_t n = buildTelemetry();
            if (n) {
                for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
                    if (s_ws.ready(i)) wsSendTelemetry(i, n, now);
                s_st.telSent++;
            }
        }
        xQueueOverwrite(s_status, &s_st);
        vTaskDelay(pdMS_TO_TICKS(WEB_TASK_PERIOD_MS));
    }
}

bool webTaskStart() {
    s_status = xQueueCreate(1, sizeof(WebStatus));
    if (s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(webTask, "web", WEB_TASK_STACK, nullptr, WEB_TASK_PRIO, nullptr, WEB_TASK_CORE) ==
           pdPASS;
}

bool webCurrent(WebStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
