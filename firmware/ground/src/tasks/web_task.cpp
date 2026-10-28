// web_task.cpp — ver web_task.h
#include "web_task.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <atomic>
#include <string.h>
#include <WebSocketsServer.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include "alarm_task.h"
#include "command_task.h"
#include "config.h"
#include "heap_guard.h"
#include "http_static.h"
#include "link_task.h"
#include "tcp_conn.h"
#include "web_json.h"
#include "ws_frame.h"
#include "ws_outbox.h"

static WiFiServer s_http(HTTP_PORT);

// O WiFiClient::connected() do núcleo testa a conexão com um recv de 0 bytes; com dados recebidos e ainda não lidos
// (o pong da página logo depois do handshake, um comando, os cabeçalhos de uma requisição), o recv volta 0 sem
// mexer no errno, e o connected() decide pelo errno que sobrou de outra operação da task. Um ECONNRESET deixado por
// uma conexão HTTP fechada pelo navegador fazia a biblioteca descartar um WebSocket saudável (bancada de
// 24/10/2026: conectou e caiu 37 ms depois, sem ser derrubado pela task). Antes de cada consulta, o errno fica em
// "nada pendente"; uma conexão fechada de verdade aparece no próximo recv ou send que falhar (o errno é por task)
static inline void errnoNeutral() { errno = EWOULDBLOCK; }
// WebSocketsServer com acesso ao socket de cada cliente. A biblioteca cuida da conexão (handshake, recepção dos
// comandos, desconexão); o envio é da task, direto no socket e sem esperar (lib/ws_frame). O sendTXT da biblioteca
// passa pelo WiFiClient::write, que espera até 1 s por tentativa, com até 10 tentativas: um cliente que some (PC
// saindo e voltando à rede, celular de tela apagada) travaria a task inteira. Nem a checagem de espaço com select()
// garantia: na bancada de 24/10/2026 uma escrita esperou 2 s mesmo assim. Fica com a biblioteca só o que ela
// escreve sozinha (resposta do handshake, pong e quadro de fechamento), raro e com o buffer quase vazio
class WsServer : public WebSocketsServer {
public:
    using WebSocketsServer::WebSocketsServer;
    // Handshake concluído
    bool ready(uint8_t num) {
        errnoNeutral();
        return clientIsConnected(num) && _clients[num].status == WSC_CONNECTED;
    }
    int fd(uint8_t num) { return _clients[num].tcp != nullptr ? _clients[num].tcp->fd() : -1; }
    // Derruba com reset, sem o quadro de fechamento (que esperaria pelo buffer cheio) e sem deixar a conexão
    // retransmitindo o que estava pendente (tcp_conn.h); o loop() da biblioteca libera a vaga
    bool drop(uint8_t num) {
        if (_clients[num].tcp == nullptr) return false;
        const bool reset = tcpReset(_clients[num].tcp->fd());
        _clients[num].tcp->stop();
        return reset;
    }
};

static_assert(WEBSOCKETS_SERVER_CLIENT_MAX == AP_MAX_CLIENTS,
              "WEBSOCKETS_SERVER_CLIENT_MAX (platformio.ini) diferente do AP_MAX_CLIENTS (config.h)");
static_assert(2 + AP_MAX_CLIENTS + HTTP_MAX_CONNS <= CONFIG_LWIP_MAX_SOCKETS - 2,
              "sockets demais para o SDK: folga de 2 para recarregamentos");

static WsServer s_ws(WS_PORT);
static uint32_t s_wsOkMs[WEBSOCKETS_SERVER_CLIENT_MAX];   // último progresso no envio de cada cliente
static uint32_t s_wsIp[WEBSOCKETS_SERVER_CLIENT_MAX];
static uint32_t s_wsSent[WEBSOCKETS_SERVER_CLIENT_MAX], s_wsSkipped[WEBSOCKETS_SERVER_CLIENT_MAX];
static int s_wsInFlight[WEBSOCKETS_SERVER_CLIENT_MAX], s_wsInFlightMax[WEBSOCKETS_SERVER_CLIENT_MAX];
static_assert(WS_MAX_INFLIGHT + WEB_MSG_BYTES + 4 <= TCP_SND_BUF,
              "WS_MAX_INFLIGHT mais um quadro precisa caber no buffer de envio do TCP");
typedef FrameTx<WEB_MSG_BYTES + 4> WsTx;                  // + cabeçalho do quadro (até 4 bytes)
static WsTx s_tx[WEBSOCKETS_SERVER_CLIENT_MAX];           // quadro em envio de cada cliente

// Mensagem que não pode se perder, guardada como dados (o JSON é montado na hora do envio)
struct WsOut {
    enum class Kind : uint8_t { Hello, Cmd, Refused } kind;
    CmdEventMsg ce;          // Cmd
    char key[24];            // Refused: nome do comando
    const char *reason;      // Refused: texto fixo (webCmdParseText ou literal)
    uint16_t rid;            // Refused: identificador do pedido (0 = nenhum)
};
static_assert(WS_OUTBOX_LEN >= WS_RECENT_CMDS + 2,
              "a fila precisa caber o hello, os comandos recentes e uma recusa (ressincronização)");
static OutRing<WsOut, WS_OUTBOX_LEN> s_out[WEBSOCKETS_SERVER_CLIENT_MAX];
static RecentBySeq<CmdEventMsg, WS_RECENT_CMDS> s_recent;   // último estado dos comandos recentes
static char s_tel[WEB_MSG_BYTES];                           // telemetria (s_msg: as mensagens da fila)
static QueueHandle_t s_status = nullptr;
static WebStatus s_st = {};
static char s_msg[WEB_MSG_BYTES];

// ---------- Eventos de rede (diagnóstico; a serial_task imprime) ----------

static QueueHandle_t s_netEvents = nullptr;
static std::atomic<uint32_t> s_netEventsDropped{0};

static void netEvent(NetEvKind kind, uint8_t slot = 0, uint32_t ip = 0, uint16_t code = 0, const char *path = nullptr,
                     const uint8_t *mac = nullptr) {
    if (s_netEvents == nullptr) return;
    NetEventMsg m = {};
    m.kind = kind;
    m.slot = slot;
    m.code = code;
    m.atMs = millis();
    m.ip = ip;
    if (mac != nullptr) memcpy(m.mac, mac, sizeof(m.mac));
    if (path != nullptr) strlcpy(m.path, path, sizeof(m.path));
    if (xQueueSend(s_netEvents, &m, 0) != pdTRUE) s_netEventsDropped.fetch_add(1, std::memory_order_relaxed);
}

// Callback do Wi-Fi (task de eventos do núcleo): só enfileira
static void onWifiEvent(arduino_event_id_t id, arduino_event_info_t info) {
    switch (id) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        netEvent(NetEvKind::StaJoin, 0, 0, 0, nullptr, info.wifi_ap_staconnected.mac);
        break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        netEvent(NetEvKind::StaLeave, 0, 0, 0, nullptr, info.wifi_ap_stadisconnected.mac);
        break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        netEvent(NetEvKind::StaIp, 0, info.wifi_ap_staipassigned.ip.addr);
        break;
    default:
        break;
    }
}

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
    uint32_t ip;                // do celular (diagnóstico)
};

static HttpConn s_conns[HTTP_MAX_CONNS];
static uint8_t s_chunk[HTTP_CHUNK_BYTES];

static void connClose(HttpConn &c) {
    if (c.file) c.file.close();
    c.client.stop();
    c.state = ConnState::Free;
}

// Ociosa (sem requisição ou parada no envio): nada a entregar, encerra com reset (tcp_conn.h)
static void connAbort(HttpConn &c) {
    if (tcpReset(c.client.fd())) s_st.tcpResets++;
    connClose(c);
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

static int respondRequest(HttpConn &c, HttpParse r, const char *path) {
    s_st.httpReqs++;
    if (r == HttpParse::NotGet) return respondText(c, 405, "Somente GET."), 405;
    if (r == HttpParse::TooLong) return respondText(c, 414, "Requisicao longa demais."), 414;
    if (r != HttpParse::Ok) return respondText(c, 400, "Requisicao invalida."), 400;
    if (!s_st.fsOk)
        return respondText(c, 503, "Pagina nao gravada no ESP B: rode 'pio run -e ground -t uploadfs' em firmware/ground."),
               503;
    // exists antes do open: o open de um arquivo ausente escreve um erro do núcleo na serial (o favicon.ico, que
    // todo navegador pede)
    if (LittleFS.exists(path)) c.file = LittleFS.open(path, "r");
    if (!c.file || c.file.isDirectory()) {
        if (c.file) c.file.close();
        char body[HTTP_PATH_MAX + 24];
        snprintf(body, sizeof(body), "Nao encontrado: %s", path);
        return respondText(c, 404, body), 404;
    }
    c.textLen = c.textSent = 0;
    c.headLen = httpBuildHeader(200, httpContentType(path), c.file.size(), c.head, sizeof(c.head));
    c.headSent = 0;
    c.state = ConnState::Sending;
    return 200;
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
    const uint8_t slot = (uint8_t)(&c - s_conns);
    errnoNeutral();
    if (!c.client.connected() && c.client.available() <= 0) {
        if (c.state == ConnState::Reading) netEvent(NetEvKind::HttpGone, slot, c.ip);
        connClose(c);
        return;
    }
    if (now - c.lastMs > HTTP_IDLE_MS) {
        s_st.httpIdle++;
        netEvent(NetEvKind::HttpIdle, slot, c.ip, c.state == ConnState::Sending ? 1 : 0);
        connAbort(c);
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
        const int code = respondRequest(c, r, path);
        netEvent(NetEvKind::HttpReq, slot, c.ip, (uint16_t)code, r == HttpParse::Ok ? path : "?");
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
            netEvent(NetEvKind::HttpReject, 0, (uint32_t)nc.remoteIP());
            nc.stop();
            return;
        }
        s_st.httpEvicted++;
        netEvent(NetEvKind::HttpEvict, (uint8_t)(slot - s_conns), slot->ip);
        connAbort(*slot);
    }
    nc.setNoDelay(true);
    slot->client = nc;
    slot->state = ConnState::Reading;
    slot->reqLen = 0;
    slot->lastMs = now;
    slot->ip = (uint32_t)nc.remoteIP();
    netEvent(NetEvKind::HttpOpen, (uint8_t)(slot - s_conns), slot->ip);
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

// O andamento de um comando já na fila substitui o anterior no lugar (só o estado mais recente importa). O "busy"
// (recusado no próprio B por falta de lugar) sai com seq 0, que não identifica um comando: nem substitui nem é
// substituído (o seq 0 também é um seq válido de um comando de verdade)
static bool sameCmd(const WsOut &a, const WsOut &b) {
    return a.kind == WsOut::Kind::Cmd && b.kind == WsOut::Kind::Cmd && !a.ce.localFull && !b.ce.localFull &&
           a.ce.ev.seq == b.ce.ev.seq;
}

static void wsPush(uint8_t num, const WsOut &o) {
    if (o.kind == WsOut::Kind::Cmd && s_out[num].replaceFirst([&o](const WsOut &x) { return sameCmd(x, o); }, o))
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
                                cmdOriginText(o.ce.ev.origin), o.ce.ev.rid, s_msg, sizeof(s_msg));
    case WsOut::Kind::Refused:
        return webBuildCmdRefused(o.key, o.reason, o.rid, s_msg, sizeof(s_msg));
    }
    return 0;
}

static void wsForget(uint8_t num) {
    s_out[num].clear();
    s_tx[num].reset();
}

// Cliente com algo a enviar e sem progresso no envio por WS_STALL_MS: ele cai (a página reconecta sozinha e
// recebe os comandos recentes na conexão)
static void wsStalled(uint8_t num, uint32_t now) {
    if (now - s_wsOkMs[num] <= WS_STALL_MS || !s_ws.ready(num)) return;
    if (s_ws.drop(num)) s_st.tcpResets++;
    wsForget(num);
    s_st.wsDropped++;
    netEvent(NetEvKind::WsDrop, num);
}

// Escreve sem esperar o que falta do quadro do cliente. true com o quadro terminado (ou sem quadro)
static bool wsPump(uint8_t num, uint32_t now) {
    WsTx &tx = s_tx[num];
    if (tx.idle()) return true;
    const int fd = s_ws.fd(num);
    if (fd < 0) {
        netEvent(NetEvKind::WsSendErr, num, 0, 0);
        wsForget(num);
        return false;
    }
    size_t n = 0;
    const uint8_t *p = tx.pending(n);
    const int w = send(fd, p, n, MSG_DONTWAIT);
    if (w > 0) {
        tx.consumed((size_t)w);
        s_wsOkMs[num] = now;
        if (tx.partial()) {
            // Quadro pela metade no fluxo: a biblioteca escreve sozinha (pong de um ping, quadro de fechamento) e
            // os bytes dela entrariam no meio do nosso quadro. Com WS_MAX_INFLIGHT sempre sobra espaço para um
            // quadro inteiro, então isso só acontece com falta de memória no lwip; o cliente cai antes de a
            // biblioteca rodar de novo, e a página reconecta. Um quadro que não saiu nada pode esperar
            s_st.wsPartial++;
            netEvent(NetEvKind::WsPartial, num);
            if (s_ws.drop(num)) s_st.tcpResets++;
            wsForget(num);
            return false;
        }
    } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        netEvent(NetEvKind::WsSendErr, num, 0, (uint16_t)errno);
        if (s_ws.drop(num)) s_st.tcpResets++;   // conexão caiu: a biblioteca avisa a desconexão no loop()
        wsForget(num);
        return false;
    }
    return tx.idle();
}

// Um quadro novo só sai para quem tem até WS_MAX_INFLIGHT bytes sem confirmação (tcp_conn.h): um cliente que some
// prende no máximo isso, e o que estava por sair espera (telemetria é pulada, a fila não se perde)
static bool wsCanLoad(uint8_t num, uint32_t now) {
    const int inflight = tcpInFlight(s_ws.fd(num));
    // Confirmações chegando contam como progresso antes de o valor guardado ser sobrescrito: sem isso, uma queda dos
    // bytes sem confirmação ainda acima de WS_MAX_INFLIGHT (sem escrita nova) sumia aqui, e a trava, comparando
    // depois, via o cliente parado (revisão de 27/10/2026)
    if (heapProgressed(s_wsInFlight[num], inflight)) s_wsOkMs[num] = now;
    s_wsInFlight[num] = inflight;
    if (inflight > s_wsInFlightMax[num]) s_wsInFlightMax[num] = inflight;
    return inflight >= 0 && inflight <= WS_MAX_INFLIGHT;
}

// Envia na ordem o que está na fila do cliente, um quadro por vez; o que não coube espera a próxima volta.
// true com a fila vazia e o último quadro terminado
static bool wsFlush(uint8_t num, uint32_t now) {
    if (!wsPump(num, now)) {
        wsStalled(num, now);
        return false;
    }
    WsOut o;
    while (s_out[num].peek(o)) {
        if (!wsCanLoad(num, now)) {
            wsStalled(num, now);
            return false;
        }
        const size_t n = buildOut(o);
        s_out[num].pop();   // daqui em diante a mensagem está no quadro, que não se perde
        if (n == 0 || !s_tx[num].load(s_msg, n)) continue;
        if (!wsPump(num, now)) {
            wsStalled(num, now);
            return false;
        }
    }
    return true;
}

// Telemetria: substituível pela próxima, então só sai para quem não tem nada pendente nem bytes demais sem
// confirmação
static void wsSendTelemetry(uint8_t num, size_t n, uint32_t now) {
    if (!s_tx[num].idle() || !s_out[num].empty() || !wsCanLoad(num, now)) {
        s_st.wsSkipped++;
        s_wsSkipped[num]++;
        wsStalled(num, now);
        return;
    }
    if (s_tx[num].load(s_tel, n)) {
        s_wsSent[num]++;
        wsPump(num, now);
    }
}

static void sendRefused(uint8_t num, const char *cmdKey, const char *reason, uint16_t rid) {
    WsOut o = {};
    o.kind = WsOut::Kind::Refused;
    snprintf(o.key, sizeof(o.key), "%s", cmdKey);
    o.reason = reason;
    o.rid = rid;
    wsPush(num, o);
    s_st.cmdsRefused++;
}

static void onWs(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED: {
        s_wsOkMs[num] = millis();
        s_wsIp[num] = (uint32_t)s_ws.remoteIP(num);
        s_wsSent[num] = s_wsSkipped[num] = 0;
        s_wsInFlight[num] = s_wsInFlightMax[num] = 0;
        s_tx[num].reset();
        wsResync(num);   // hello e os comandos recentes
        netEvent(NetEvKind::WsOpen, num, s_wsIp[num]);
        break;
    }
    case WStype_DISCONNECTED:
        wsForget(num);
        netEvent(NetEvKind::WsClose, num);
        break;
    case WStype_TEXT: {
        uint8_t cmd;
        int32_t arg;
        WebCmdOrigin origin;
        uint16_t rid;
        char key[24];
        const WebCmdParse r =
            webParseCommand((const char *)payload, len, WEB_CMD_PIN, cmd, arg, origin, rid, key, sizeof(key));
        if (r != WebCmdParse::Ok) {
            sendRefused(num, key, webCmdParseText(r), rid);
            return;
        }
        if (!commandSubmit(cmd, arg, origin == WebCmdOrigin::Pc ? CMD_FROM_PC : CMD_FROM_WEB, rid)) {
            sendRefused(num, key, "fila de comandos cheia", rid);
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
static size_t buildTelemetry(uint32_t now) {
    WebTelemetry t = {};
    t.bMs = now;
    t.telN = s_st.telSent + 1;   // a próxima a sair; uma montagem que não coube não avança
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

// Trava de memória geral: com o ar ruim (interferência, vários celulares sumindo juntos), os buffers de
// transmissão do Wi-Fi seguram o que não sai, e o heap caiu a 18 kB na bancada de 24/10/2026 com dois celulares
// (o limite por cliente, WS_MAX_INFLIGHT, só cobre o socket). Abaixo de WEB_HEAP_PAUSE_KB, a telemetria pausa
// para todos até o heap passar de WEB_HEAP_RESUME_KB; os comandos e os eventos continuam. As páginas mostram
// "sem telemetria" enquanto isso
// Na pausa, a telemetria não sai, então "progresso" é o TCP do cliente esvaziando: sem bytes sem confirmação, ou
// com menos que na consulta anterior. Fila da aplicação vazia não basta: um cliente que sumiu durante a pausa
// seguraria os bytes dele (e a pausa) indefinidamente. Sem progresso por WS_STALL_MS, cai como fora da pausa
static void wsPausedProgress(uint32_t now) {
    static uint32_t lastCheck = 0;
    if (now - lastCheck < WEB_PAUSE_CHECK_MS) return;
    lastCheck = now;
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (!s_ws.ready(i) || !s_tx[i].idle() || !s_out[i].empty()) continue;   // pendentes: wsFlush cuida
        const int inflight = tcpInFlight(s_ws.fd(i));
        if (heapProgressed(s_wsInFlight[i], inflight)) s_wsOkMs[i] = now;
        s_wsInFlight[i] = inflight;
        wsStalled(i, now);
    }
}

static_assert(WEB_HEAP_RESUME_KB > WEB_HEAP_PAUSE_KB, "histerese da trava de memória");
// Com pouca memória, primeiro derruba o culpado (um cliente com algo pendente e parado há WEB_HEAP_STALL_MS: o
// aparelho que sumiu do canal); só sem culpado pausa a telemetria de todos (lib/heap_guard). Bancada 2, 26/10/2026:
// com 4 celulares, a pausa para todos levava ~1 a 1,7 s de telemetria por minuto
static HeapGuard s_heapGuard({WEB_HEAP_PAUSE_KB, WEB_HEAP_RESUME_KB, WEB_HEAP_STALL_MS, WEB_HEAP_DROP_GAP_MS});

static void heapGuard(uint32_t now) {
    static uint32_t pausedAt = 0;
    const uint32_t freeKb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    HeapClient clientes[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
    int n = 0;
    // Com pouca memória, a consulta do TCP de cada cliente (na thread do TCP/IP) e a decisão no máximo a cada
    // WEB_PAUSE_CHECK_MS: na primeira versão, a cada volta da task, centenas de consultas por segundo com o heap baixo
    static uint32_t lastCheck = 0;
    if (freeKb < WEB_HEAP_PAUSE_KB) {
        if (now - lastCheck < WEB_PAUSE_CHECK_MS) return;
        lastCheck = now;
        n = WEBSOCKETS_SERVER_CLIENT_MAX;
        for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
            clientes[i].ready = s_ws.ready(i);
            if (!clientes[i].ready) continue;
            // Confirmações chegando contam como progresso antes da escolha do culpado (fora da pausa, só uma escrita
            // nova atualizava o s_wsOkMs; na pausa, o wsPausedProgress rodava depois daqui)
            const int inflight = tcpInFlight(s_ws.fd(i));
            if (heapProgressed(s_wsInFlight[i], inflight)) s_wsOkMs[i] = now;
            s_wsInFlight[i] = inflight;
            clientes[i].pending = !s_tx[i].idle() || inflight > 0;
            clientes[i].lastOkMs = s_wsOkMs[i];
        }
    }
    const HeapDecision d = s_heapGuard.step(freeKb, now, clientes, n);
    switch (d.action) {
    case HeapAction::Drop:
        if (s_ws.drop((uint8_t)d.client)) s_st.tcpResets++;
        wsForget((uint8_t)d.client);
        s_st.wsHeapDrops++;
        netEvent(NetEvKind::WsHeapDrop, (uint8_t)d.client, d.stalledMs, (uint16_t)freeKb);
        break;
    case HeapAction::Pause:
        s_st.heapPaused = true;
        s_st.heapPauses++;
        pausedAt = now;
        netEvent(NetEvKind::HeapPause, 0, 0, (uint16_t)freeKb);
        break;
    case HeapAction::Resume:
        s_st.heapPaused = false;
        s_st.heapPausedMs += now - pausedAt;
        netEvent(NetEvKind::HeapResume, 0, now - pausedAt, (uint16_t)freeKb);
        break;
    case HeapAction::None:
        break;
    }
}

static void webTask(void *) {
    // Sem formatar: um LittleFS ausente ou corrompido só desliga a página (a mensagem explica o uploadfs)
    s_st.fsOk = LittleFS.begin(false) && LittleFS.exists("/index.html");
    WiFi.onEvent(onWifiEvent);
    s_http.begin();   // todas as rotas: arquivos do LittleFS ("/" = index.html)
    s_http.setNoDelay(true);
    s_ws.begin();
    s_ws.onEvent(onWs);
    const uint32_t telPeriodMs = 1000 / WS_RATE_HZ;
    uint32_t lastTelMs = 0;
    for (;;) {
        httpLoop();
        errnoNeutral();   // o loop() da biblioteca consulta o connected() de cada cliente
        s_ws.loop();
        CmdEventMsg ce;
        while (commandWebEventReceive(ce)) {
            // O "busy" (seq 0, sem lugar no B) não entra nos recentes: o seq 0 sobrescreveria o de um comando de
            // verdade. Ele vai só uma vez, na hora; numa fila refeita, não volta
            if (!ce.localFull) s_recent.upsert(ce.ev.seq, ce);
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
        heapGuard(now);
        if (s_st.heapPaused) {
            wsPausedProgress(now);
        } else if (s_st.clients > 0 && now - lastTelMs >= telPeriodMs) {
            lastTelMs = now;
            const size_t n = buildTelemetry(now);
            if (n) {
                for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
                    if (s_ws.ready(i)) wsSendTelemetry(i, n, now);
                s_st.telSent++;
            }
        }
        for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
            WsClientStat &cs = s_st.ws[i];
            cs.on = s_ws.ready(i);
            cs.ip = s_wsIp[i];
            cs.sent = s_wsSent[i];
            cs.skipped = s_wsSkipped[i];
            cs.inFlight = s_wsInFlight[i];
            cs.inFlightMax = s_wsInFlightMax[i];
            cs.idleMs = cs.on ? millis() - s_wsOkMs[i] : 0;
        }
        xQueueOverwrite(s_status, &s_st);
        vTaskDelay(pdMS_TO_TICKS(WEB_TASK_PERIOD_MS));
    }
}

bool webTaskStart() {
    s_status = xQueueCreate(1, sizeof(WebStatus));
    s_netEvents = xQueueCreate(WEB_NET_EVENT_QUEUE_LEN, sizeof(NetEventMsg));
    if (s_status == nullptr || s_netEvents == nullptr) return false;
    return xTaskCreatePinnedToCore(webTask, "web", WEB_TASK_STACK, nullptr, WEB_TASK_PRIO, nullptr, WEB_TASK_CORE) ==
           pdPASS;
}

bool webCurrent(WebStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }

bool webNetEventReceive(NetEventMsg &out) {
    return s_netEvents != nullptr && xQueueReceive(s_netEvents, &out, 0) == pdTRUE;
}

uint32_t webNetEventsDropped() { return s_netEventsDropped.load(std::memory_order_relaxed); }
