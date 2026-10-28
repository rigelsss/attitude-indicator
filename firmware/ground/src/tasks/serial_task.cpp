// serial_task.cpp — ver serial_task.h
#include "serial_task.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <esp_timer.h>
#include "config.h"
#include <esp_heap_caps.h>
#include "alarm_task.h"
#include "command_task.h"
#include "display_task.h"
#include "input_task.h"
#include "link_task.h"
#include "oled_task.h"
#include "radio_rx.h"
#include "web_task.h"

static uint32_t rxRefused(const RxCounters &c) { return c.badSize + c.badMagic + c.badVersion + c.badType; }

static float lossPct(const LinkCounts &t) {
    const uint32_t expected = t.received + t.lost;
    return expected > 0 ? 100.0f * t.lost / expected : 0.0f;
}

static void printEvent(const LinkEventMsg &e) {
    const LinkUpdate &u = e.u;
    if (u.events & LINK_EV_FIRST) Serial.printf("[link] %lu ms: estabelecido (primeiro pacote desde o boot)\n", (unsigned long)e.atMs);
    if (u.events & LINK_EV_LOST)
        Serial.printf("[link] %lu ms: PERDIDO (%lu ms sem pacotes)\n", (unsigned long)e.atMs,
                      (unsigned long)(u.outageUs / 1000));
    if (u.events & LINK_EV_RESTORED)
        Serial.printf("[link] %lu ms: restabelecido depois de %lu ms sem pacotes\n", (unsigned long)e.atMs,
                      (unsigned long)(u.outageUs / 1000));
    if (u.events & LINK_EV_RESYNC)
        Serial.printf("[link] %lu ms: ressincronizado: seq %u -> %u, t_ms do A %lu -> %lu (A reiniciado ou falta "
                      "longa; perdas não contadas)\n",
                      (unsigned long)e.atMs, u.seqFrom, u.seqTo, (unsigned long)u.tFrom, (unsigned long)u.tTo);
}

static void printAlarmEvent(const AlarmEventMsg &e) {
    switch (e.type) {
    case AlarmEventMsg::Changed:
        if (e.kind == AlarmKind::None) {
            Serial.printf("[alarme] %lu ms: terminou\n", (unsigned long)e.atMs);
        } else if (e.kind == AlarmKind::LinkLost) {
            Serial.printf("[alarme] %lu ms: %s\n", (unsigned long)e.atMs, alarmKindText(e.kind));
        } else {
            Serial.printf("[alarme] %lu ms: %s (roll %+.1f, pitch %+.1f, valid 0x%04X, status 0x%04X)\n",
                          (unsigned long)e.atMs, alarmKindText(e.kind), e.rollDeg, e.pitchDeg, e.valid, e.status);
        }
        break;
    case AlarmEventMsg::Acked:
        Serial.printf("[alarme] %lu ms: reconhecido: %s\n", (unsigned long)e.atMs, alarmKindText(e.kind));
        break;
    case AlarmEventMsg::NothingToAck:
        Serial.printf("[alarme] %lu ms: nada a reconhecer\n", (unsigned long)e.atMs);
        break;
    }
}

static void printInputEvent(const InputEventMsg &e) {
    switch (e.type) {
    case InputEventMsg::Click:
        Serial.printf("[enc] %lu ms: clique %s, página %u/%d\n", (unsigned long)e.atMs,
                      e.dir > 0 ? "horário" : "anti-horário", e.page + 1, OLED_PAGES);
        break;
    case InputEventMsg::ShortPress:
        Serial.printf("[enc] %lu ms: botão curto: reconhecer o alarme\n", (unsigned long)e.atMs);
        break;
    case InputEventMsg::LongPress:
        Serial.printf("[enc] %lu ms: pressão longa na página %u: %s\n", (unsigned long)e.atMs, e.page + 1,
                      commandText((uint8_t)e.dir));
        break;
    case InputEventMsg::AckDropped:
        Serial.printf("[enc] %lu ms: botão curto descartado: fila de reconhecimento do alarme cheia\n",
                      (unsigned long)e.atMs);
        break;
    case InputEventMsg::CmdDropped:
        Serial.printf("[enc] %lu ms: pressão longa descartada: fila de comandos cheia\n", (unsigned long)e.atMs);
        break;
    }
}

static void printCommandEvent(const CmdEventMsg &m) {
    const CmdEvent &e = m.ev;
    if (m.localFull) {
        Serial.printf("[cmd] %lu ms: '%s' (%s) recusado no B: comandos demais em curso\n", (unsigned long)m.atMs,
                      commandText(e.cmd), cmdOriginText(e.origin));
        return;
    }
    if (e.state == CmdState::Sending) {
        if (m.attempt <= 1)
            Serial.printf("[cmd] %lu ms: '%s' (%s, seq %u%s) enviado\n", (unsigned long)m.atMs, commandText(e.cmd),
                          cmdOriginText(e.origin), e.seq, e.cmd == CMD_SET_RATE_HZ ? ", taxa" : "");
        else
            Serial.printf("[cmd] %lu ms: '%s' (seq %u) sem ACK: reenvio %u de %d\n", (unsigned long)m.atMs,
                          commandText(e.cmd), e.seq, m.attempt, CMD_MAX_TRIES);
        if (m.attempt <= 1 && e.cmd == CMD_SET_RATE_HZ) Serial.printf("[cmd]   taxa pedida: %ld Hz\n", (long)e.arg);
        return;
    }
    Serial.printf("[cmd] %lu ms: '%s' (seq %u): %s\n", (unsigned long)m.atMs, commandText(e.cmd), e.seq,
                  cmdStateText(e.state));
}

static void printNetEvent(const NetEventMsg &m) {
    const String ip = IPAddress(m.ip).toString();
    const unsigned long t = (unsigned long)m.atMs;
    switch (m.kind) {
    case NetEvKind::StaJoin:
    case NetEvKind::StaLeave:
        Serial.printf("[ap] %lu ms: celular %02X:%02X:%02X:%02X:%02X:%02X %s | no AP agora: %d\n", t, m.mac[0], m.mac[1],
                      m.mac[2], m.mac[3], m.mac[4], m.mac[5], m.kind == NetEvKind::StaJoin ? "entrou" : "saiu",
                      WiFi.softAPgetStationNum());
        break;
    case NetEvKind::StaIp:
        Serial.printf("[ap] %lu ms: IP %s entregue pelo DHCP\n", t, ip.c_str());
        break;
    case NetEvKind::HttpOpen:
        Serial.printf("[http] %lu ms: conexão de %s (vaga %u)\n", t, ip.c_str(), m.slot);
        break;
    case NetEvKind::HttpReq:
        Serial.printf("[http] %lu ms: %s GET %s -> %u (vaga %u)\n", t, ip.c_str(), m.path, m.code, m.slot);
        break;
    case NetEvKind::HttpIdle:
        Serial.printf("[http] %lu ms: %s fechada por ociosidade (vaga %u, %s)\n", t, ip.c_str(), m.slot,
                      m.code ? "parada no envio" : "sem requisição");
        break;
    case NetEvKind::HttpGone:
        Serial.printf("[http] %lu ms: %s fechou sem requisição (vaga %u)\n", t, ip.c_str(), m.slot);
        break;
    case NetEvKind::HttpEvict:
        Serial.printf("[http] %lu ms: %s ociosa fechada para dar lugar (vaga %u)\n", t, ip.c_str(), m.slot);
        break;
    case NetEvKind::HttpReject:
        Serial.printf("[http] %lu ms: %s recusada: todas as vagas enviando\n", t, ip.c_str());
        break;
    case NetEvKind::WsOpen:
        Serial.printf("[ws] %lu ms: cliente %u conectado (%s)\n", t, m.slot, ip.c_str());
        break;
    case NetEvKind::WsClose:
        Serial.printf("[ws] %lu ms: cliente %u desconectado\n", t, m.slot);
        break;
    case NetEvKind::WsPartial:
        Serial.printf("[ws] %lu ms: cliente %u: quadro saiu pela metade (falta de memória no lwip); derrubado\n", t,
                      m.slot);
        break;
    case NetEvKind::HeapPause:
        Serial.printf("[web] %lu ms: telemetria PAUSADA para todos: heap livre %u kB (abaixo de %d kB)\n", t, m.code,
                      WEB_HEAP_PAUSE_KB);
        break;
    case NetEvKind::HeapResume:
        Serial.printf("[web] %lu ms: telemetria retomada: heap livre %u kB, pausada por %lu ms\n", t, m.code,
                      (unsigned long)m.ip);
        break;
    case NetEvKind::WsSendErr:
        if (m.code == 0)
            Serial.printf("[ws] %lu ms: cliente %u: sem socket no envio\n", t, m.slot);
        else
            Serial.printf("[ws] %lu ms: cliente %u: erro no envio (errno %u: %s); descartado\n", t, m.slot, m.code,
                          strerror(m.code));
        break;
    case NetEvKind::WsDrop:
        Serial.printf("[ws] %lu ms: cliente %u derrubado: %d ms sem espaço no envio\n", t, m.slot, WS_STALL_MS);
        break;
    case NetEvKind::WsHeapDrop:
        Serial.printf("[ws] %lu ms: cliente %u derrubado: trava de memória (heap livre %u kB, %lu ms sem progresso)\n",
                      t, m.slot, m.code, (unsigned long)m.ip);
        break;
    }
}

// Verificação de integridade do heap a cada WEB_HEAP_CHECK_MS (investigação do CORRUPT HEAP de 26/10/2026): a
// corrupção só era vista quando o driver Wi-Fi liberava o bloco vizinho; aqui, a primeira verificação que falha diz
// quando ela aconteceu (entre a última ok e esta), para cruzar com os eventos [ap], [ws] e [web] em volta
static uint32_t s_heapChecks = 0, s_heapOkMs = 0, s_heapBadMs = 0;
static bool s_heapBad = false;

static void heapIntegrityStep(uint32_t now) {
    static uint32_t ultima = 0;
    if (WEB_HEAP_CHECK_MS == 0 || s_heapBad || now - ultima < WEB_HEAP_CHECK_MS) return;
    ultima = now;
    if (heap_caps_check_integrity_all(false)) {
        s_heapChecks++;
        s_heapOkMs = now;
        return;
    }
    s_heapBad = true;
    s_heapBadMs = now;
    Serial.printf("[heap] %lu ms: CORRUPÇÃO DO HEAP detectada (última verificação ok em %lu ms); detalhes:\n",
                  (unsigned long)now, (unsigned long)s_heapOkMs);
    Serial.flush();
    heap_caps_check_integrity_all(true);   // imprime o bloco e o tipo de dano (direto na UART)
}

static void heapIntegrityLine() {
    if (WEB_HEAP_CHECK_MS == 0) {
        Serial.println("heap: verificação de integridade desligada");
    } else if (s_heapBad) {
        Serial.printf("heap: CORROMPIDO desde %lu ms (última verificação ok em %lu ms)\n", (unsigned long)s_heapBadMs,
                      (unsigned long)s_heapOkMs);
    } else {
        Serial.printf("heap: íntegro | %lu verificações ok, a cada %d ms\n", (unsigned long)s_heapChecks,
                      WEB_HEAP_CHECK_MS);
    }
}

// Taxas aceitas pelo A, em ciclo pela tecla f (50 → 25 → 20 → 10 → 50)
static int32_t nextRate(int32_t cur) { return cur == 50 ? 25 : cur == 25 ? 20 : cur == 20 ? 10 : 50; }

static void printReport(const LinkReport &r) {
    const LinkCounts &w = r.win.counts;
    // O instante (millis do B) no começo, como nos eventos: o analisar_b.py cruza os resumos com as entradas e
    // saídas dos aparelhos no AP
    Serial.printf("[r] %lu ms: link %s | rec %lu em %lu ms | perdidos %lu | dup %lu | fora de ordem %lu | ressinc %lu | "
                  "recusados %lu | outro remetente %lu | fila cheia %lu",
                  (unsigned long)millis(), linkStateText(r.state), (unsigned long)w.received, (unsigned long)r.windowMs, (unsigned long)w.lost,
                  (unsigned long)w.duplicates, (unsigned long)w.late, (unsigned long)w.resyncs,
                  (unsigned long)rxRefused(r.rxWin),
                  (unsigned long)r.rxWin.foreign, (unsigned long)r.rxWin.queueFull);
    if (r.win.intervals > 0)
        Serial.printf(" | intervalo %.1f/%.1f/%.1f ms (no A %lu/%lu ms)", r.win.intervalMinUs / 1000.0f,
                      (float)r.win.intervalSumUs / r.win.intervals / 1000.0f, r.win.intervalMaxUs / 1000.0f,
                      (unsigned long)r.win.srcIntervalMinMs, (unsigned long)r.win.srcIntervalMaxMs);
    else
        Serial.print(" | intervalo -");
    if (r.havePacket)
        Serial.printf(" | idade %lu ms | atraso %lu (máx %lu) ms | seq %u", (unsigned long)r.ageMs,
                      (unsigned long)r.delayMs, (unsigned long)r.delayMaxMs, r.lastSeq);
    Serial.printf(" | total: %lu rec, %lu perd (%.3f %%), %lu quedas", (unsigned long)r.totals.received,
                  (unsigned long)r.totals.lost, lossPct(r.totals), (unsigned long)r.totals.timeouts);
    DisplayStatus ds;   // quadros do TFT no mesmo resumo: o passo 6 compara a recepção com e sem carga do TFT
    if (displayCurrent(ds) && ds.spriteOk)
        Serial.printf(" | tft %.0f q/s, maior %.0f ms", ds.fps, ds.maxFrameUs / 1000.0f);
    // heap no resumo: mostra o momento em que a memória cai (bancada de 24/10/2026)
    Serial.printf(" | heap %u kB (bloco %u kB)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    Serial.println();
}

static void printPacket(const RxItem &it) {
    const TelemetryPacket &p = it.pkt;
    const uint64_t nowUs = (uint64_t)esp_timer_get_time();
    const unsigned long ageMs = nowUs > it.arrivalUs ? (unsigned long)((nowUs - it.arrivalUs) / 1000) : 0;
    Serial.printf("[a] seq %u | t %lu ms | roll %.2f pitch %.2f | proa %.1f | alt %.2f m | %.2f hPa | %.1f C | "
                  "status 0x%04X | valid 0x%04X | loop %u us | idade %lu ms | atraso %lu ms\n",
                  p.hdr.seq, (unsigned long)p.t_ms, p.roll, p.pitch, p.heading, p.altitude_m, p.pressure_hpa,
                  p.temp_c, p.status, p.valid, p.loop_us, ageMs, (unsigned long)it.delayMs);
}

static void printState(bool streamR, bool streamA) {
    extern const char *const BUILD_STAMP;
    const RadioInfo &ri = radioInfo();
    Serial.println("---------------- ESP B (solo) ----------------");
    Serial.printf("firmware: compilado em %s | contrato v%d, TelemetryPacket %u bytes\n", BUILD_STAMP,
                  TELEMETRY_VERSION, (unsigned)sizeof(TelemetryPacket));
    if (ri.active)
        Serial.printf("rádio: ativo | MAC STA %s (recebe) | MAC AP %s | AP \"%s\" | canal %u | peer "
                      "%02X:%02X:%02X:%02X:%02X:%02X | DHCP .%u a .%u (alternado, boot %lu)\n",
                      ri.staMac, ri.apMac, AP_SSID, ri.channel, PEER_MAC[0], PEER_MAC[1], PEER_MAC[2], PEER_MAC[3],
                      PEER_MAC[4], PEER_MAC[5], ri.dhcpInicio, ri.dhcpInicio + 10, (unsigned long)ri.boots);
    else
        Serial.println("rádio: DESLIGADO (falha no boot; ver as linhas [radio])");
    Serial.printf("link: timeout %d ms, ressincroniza acima de %d pacotes, fora de ordem até %d pacotes e %d ms, "
                  "resumo a cada %d ms\n",
                  LINK_TIMEOUT_MS, LINK_RESYNC_GAP, LINK_LATE_MAX_PKTS, LINK_LATE_MAX_MS, LINK_REPORT_MS);
    Serial.printf("atraso: DADO ATRASADO acima de %d ms; base = menor offset em %d faixas de %d ms\n",
                  LINK_STALE_DELAY_MS, DELAY_BUCKETS, DELAY_BUCKET_MS);
    LinkReport r;
    if (linkLatestReport(r)) {
        Serial.printf("estado (último resumo): %s", linkStateText(r.state));
        if (r.havePacket)
            Serial.printf(" | último seq %u, t_ms do A %lu, idade %lu ms, atraso %lu ms", r.lastSeq,
                          (unsigned long)r.lastTms, (unsigned long)r.ageMs, (unsigned long)r.delayMs);
        Serial.println();
        Serial.printf("desde o boot: %lu rec | %lu perdidos (%.3f %%) | %lu dup | %lu fora de ordem | %lu ressinc | "
                      "%lu quedas\n",
                      (unsigned long)r.totals.received, (unsigned long)r.totals.lost, lossPct(r.totals),
                      (unsigned long)r.totals.duplicates, (unsigned long)r.totals.late, (unsigned long)r.totals.resyncs,
                      (unsigned long)r.totals.timeouts);
    } else {
        Serial.println("estado: ainda sem resumo");
    }
    const RxCounters c = radioRxCounters();
    Serial.printf("recusados desde o boot: tamanho %lu, magic %lu, versão %lu, tipo %lu | outro remetente %lu | "
                  "fila cheia %lu | eventos descartados: link %lu, alarme %lu\n",
                  (unsigned long)c.badSize, (unsigned long)c.badMagic, (unsigned long)c.badVersion,
                  (unsigned long)c.badType, (unsigned long)c.foreign, (unsigned long)c.queueFull,
                  (unsigned long)linkEventsDropped(), (unsigned long)alarmEventsDropped());
    AlarmStatus al;
    if (alarmCurrent(al))
        Serial.printf("alarme: %s%s | LED do link %s | limites: aviso bank %.0f, alarme bank %.0f, pitch +%.0f/-%.0f, "
                      "histerese %.0f graus\n",
                      alarmKindText(al.kind), al.acked ? " (reconhecido)" : "", al.linkLed ? "aceso" : "apagado",
                      ALARM_BANK_CAUTION_DEG, ALARM_BANK_DEG, ALARM_PITCH_UP_DEG, ALARM_PITCH_DOWN_DEG, ALARM_HYST_DEG);
    else
        Serial.println("alarme: task não iniciada");
    InputStatus in;
    if (inputCurrent(in))
        Serial.printf("encoder: página %u/%d | cliques horário %lu, anti-horário %lu | inválidas %lu | fila da ISR "
                      "cheia %lu (lacunas %lu) | eventos descartados %lu\n",
                      in.page + 1, OLED_PAGES, (unsigned long)in.clicksCw, (unsigned long)in.clicksCcw,
                      (unsigned long)in.invalid, (unsigned long)in.isrDropped, (unsigned long)in.gaps,
                      (unsigned long)inputEventsDropped());
    else
        Serial.println("encoder: task não iniciada");
    OledStatus ol;
    if (!oledCurrent(ol))
        Serial.println("oled: task não iniciada");
    else if (ol.present)
        Serial.printf("oled: presente | página %u/%d | último quadro %lu us | quadros %lu | proa %s\n", ol.page + 1,
                      OLED_PAGES, (unsigned long)ol.frameUs, (unsigned long)ol.frames,
                      OLED_HEADING_TRUE ? "verdadeira (VERD)" : "magnética (MAG)");
    else
        Serial.printf("oled: AUSENTE em 0x%02X (nova tentativa a cada %d ms)\n", ADDR_OLED_DATA, OLED_RETRY_MS);
    DisplayStatus ds;
    if (!displayCurrent(ds))
        Serial.println("tft: task não iniciada (ou ainda sem janela de 1 s)");
    else if (!ds.spriteOk)
        Serial.println("tft: SEM SPRITE (sem memória no boot): só a mensagem de erro");
    else
        Serial.printf("tft: %.1f quadros/s (meta %d) | desenho %.1f ms, envio %.1f ms, maior quadro %.1f ms | "
                      "quadros %lu, acima do período %lu\n",
                      ds.fps, DISPLAY_FPS, ds.drawUs / 1000.0f, ds.pushUs / 1000.0f, ds.maxFrameUs / 1000.0f,
                      (unsigned long)ds.frames, (unsigned long)ds.overruns);
    const CmdStats cs = commandStats();
    Serial.printf("comandos: pedidos %lu | envios %lu (reconsultas %lu, falhas %lu) | ACKs casados %lu, ignorados %lu "
                  "| recusados no B %lu | ACKs do rádio: %lu, inválidos %lu, fila cheia %lu\n",
                  (unsigned long)cs.submitted, (unsigned long)cs.sends, (unsigned long)cs.polls, (unsigned long)cs.sendErrors,
                  (unsigned long)cs.acksMatched, (unsigned long)cs.acksIgnored, (unsigned long)cs.localRejects,
                  (unsigned long)c.acks, (unsigned long)c.acksBad, (unsigned long)c.acksDropped);
    WebStatus ws;
    if (webCurrent(ws))
        Serial.printf("página: %s | http://192.168.4.1 (WebSocket :%d) | clientes %u | telemetria %d Hz, %lu enviadas | comandos "
                      "aceitos %lu, recusados %lu | PIN %s\n"
                      "ws: telemetrias puladas %lu | fila: refeitas %lu | derrubados por quadro pela metade %lu | clientes derrubados "
                      "%lu\n"
                      "ap: celulares na rede %d/%d | http: requisições %lu | conexões abertas %u/%d | fechadas "
                      "ociosas %lu, para dar lugar %lu | recusadas %lu\n",
                      ws.fsOk ? "gravada" : "NÃO GRAVADA (uploadfs)", WS_PORT, ws.clients, WS_RATE_HZ,
                      (unsigned long)ws.telSent,
                      (unsigned long)ws.cmdsOk, (unsigned long)ws.cmdsRefused, WEB_CMD_PIN[0] ? "exigido" : "desligado",
                      (unsigned long)ws.wsSkipped, (unsigned long)ws.wsResyncs, (unsigned long)ws.wsPartial,
                      (unsigned long)ws.wsDropped,
                      WiFi.softAPgetStationNum(), AP_MAX_STATIONS, (unsigned long)ws.httpReqs, ws.httpOpen,
                      HTTP_MAX_CONNS, (unsigned long)ws.httpIdle,
                      (unsigned long)ws.httpEvicted, (unsigned long)ws.httpRejected);
    if (webCurrent(ws)) {
        Serial.printf("tcp: conexões encerradas com reset %lu | trava de memória: %s, culpados derrubados %lu, pausas "
                      "%lu, tempo pausado %lu ms (derruba quem está parado, ou pausa; abaixo de %d kB, volta acima "
                      "de %d kB)\n",
                      (unsigned long)ws.tcpResets, ws.heapPaused ? "PAUSADA" : "livre", (unsigned long)ws.wsHeapDrops,
                      (unsigned long)ws.heapPauses, (unsigned long)ws.heapPausedMs, WEB_HEAP_PAUSE_KB,
                      WEB_HEAP_RESUME_KB);
        heapIntegrityLine();
        for (int i = 0; i < AP_MAX_CLIENTS; i++) {
            const WsClientStat &cs = ws.ws[i];
            if (!cs.on) continue;
            Serial.printf("ws cliente %d: %s | telemetrias enviadas %lu, puladas %lu | último progresso há %lu ms | "
                          "sem confirmação %ld B (máx %ld, limite %d)\n",
                          i, IPAddress(cs.ip).toString().c_str(), (unsigned long)cs.sent, (unsigned long)cs.skipped,
                          (unsigned long)cs.idleMs, (long)cs.inFlight, (long)cs.inFlightMax, WS_MAX_INFLIGHT);
        }
    }
    else
        Serial.println("página: task não iniciada");
    Serial.printf("fluxos: r %s, a %s | heap livre %u kB, maior bloco %u kB\n", streamR ? "ligado" : "desligado",
                  streamA ? "ligado" : "desligado", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    Serial.println("----------------------------------------------");
}

static void printHelp() {
    Serial.println("[B] teclas: c = estado | r = resumo por segundo | a = fluxo de pacotes (10 Hz) | k = reconhecer o "
                   "alarme (o mesmo que o botão do encoder)");
    Serial.println("[B] comandos para o A: g = calibrar giroscópio | m = calibrar magnetômetro | z = zerar atitude | "
                   "0 = zerar altitude | f = próxima taxa (50, 25, 20, 10 Hz) | X = taxa inválida (teste da recusa)");
}

static void serialTask(void *) {
    bool streamR = true;   // o resumo começa ligado: é o que a bancada do rádio acompanha
    bool streamA = false;
    uint32_t reportSeen = 0, droppedSeen = 0, alarmDroppedSeen = 0, inputDroppedSeen = 0, cmdDroppedSeen = 0,
             netDroppedSeen = 0;
    int32_t rate = 50;   // última taxa pedida pela tecla f (o A começa em TX_RATE_HZ, 50 Hz)
    int oledSeen = -1;   // presença do OLED já avisada: −1 ainda não lida, 0 ausente, 1 presente
    uint16_t lastSeqPrinted = 0;
    bool printedAny = false;
    TickType_t lastPkt = xTaskGetTickCount();
    for (;;) {
        heapIntegrityStep(millis());
        LinkEventMsg e;
        while (linkEventReceive(e)) printEvent(e);
        const uint32_t dropped = linkEventsDropped();
        if (dropped != droppedSeen) {
            Serial.printf("[link] %lu eventos descartados: fila de eventos cheia\n", (unsigned long)(dropped - droppedSeen));
            droppedSeen = dropped;
        }

        AlarmEventMsg ae;
        while (alarmEventReceive(ae)) printAlarmEvent(ae);
        const uint32_t alarmDropped = alarmEventsDropped();
        if (alarmDropped != alarmDroppedSeen) {
            Serial.printf("[alarme] %lu mudanças descartadas: fila de alarmes cheia (o c mostra o estado atual)\n",
                          (unsigned long)(alarmDropped - alarmDroppedSeen));
            alarmDroppedSeen = alarmDropped;
        }
        OledStatus ol;
        if (oledCurrent(ol) && (oledSeen < 0 || ol.present != (oledSeen == 1))) {
            if (oledSeen >= 0 || !ol.present)   // no boot, só avisa se faltar
                Serial.printf("[oled] %s\n", ol.present ? "presente" : "AUSENTE: sem resposta em 0x3C");
            oledSeen = ol.present ? 1 : 0;
        }
        CmdEventMsg ce;
        while (commandEventReceive(ce)) printCommandEvent(ce);
        const uint32_t cmdDropped = commandEventsDropped();
        if (cmdDropped != cmdDroppedSeen) {
            Serial.printf("[cmd] %lu mudanças de comando descartadas: fila cheia\n",
                          (unsigned long)(cmdDropped - cmdDroppedSeen));
            cmdDroppedSeen = cmdDropped;
        }
        NetEventMsg ne;
        while (webNetEventReceive(ne)) printNetEvent(ne);
        const uint32_t netDropped = webNetEventsDropped();
        if (netDropped != netDroppedSeen) {
            Serial.printf("[rede] %lu eventos de rede descartados: fila cheia\n",
                          (unsigned long)(netDropped - netDroppedSeen));
            netDroppedSeen = netDropped;
        }
        InputEventMsg ie;
        while (inputEventReceive(ie)) printInputEvent(ie);
        const uint32_t inputDropped = inputEventsDropped();
        if (inputDropped != inputDroppedSeen) {
            Serial.printf("[enc] %lu eventos descartados: fila do encoder cheia (o c mostra a página atual)\n",
                          (unsigned long)(inputDropped - inputDroppedSeen));
            inputDroppedSeen = inputDropped;
        }

        LinkReport r;
        if (linkLatestReport(r) && r.index != reportSeen) {
            reportSeen = r.index;
            if (streamR) printReport(r);
        }

        if (streamA && xTaskGetTickCount() - lastPkt >= pdMS_TO_TICKS(STREAM_PKT_PERIOD_MS)) {
            lastPkt = xTaskGetTickCount();
            RxItem it;
            // Só pacotes novos: com o link parado, o fluxo fica em silêncio (o r mostra o estado)
            if (linkLatestPacket(it) && (!printedAny || it.pkt.hdr.seq != lastSeqPrinted)) {
                printPacket(it);
                lastSeqPrinted = it.pkt.hdr.seq;
                printedAny = true;
            }
        }

        while (Serial.available()) {
            const char k = (char)Serial.read();
            switch (k) {
            case 'c': printState(streamR, streamA); break;
            case 'r':
                streamR = !streamR;
                Serial.printf("[B] resumo por segundo %s\n", streamR ? "ligado" : "desligado");
                break;
            case 'k':
                if (!alarmAcknowledge()) Serial.println("[B] tecla k descartada: fila de reconhecimento cheia");
                break;
            case 'g':
            case 'm':
            case 'z':
            case '0':
            case 'f':
            case 'X': {
                uint8_t cmd = 0;
                int32_t arg = 0;
                switch (k) {
                case 'g': cmd = CMD_CALIBRATE_GYRO; break;
                case 'm': cmd = CMD_CALIBRATE_MAG; break;
                case 'z': cmd = CMD_ZERO_ATTITUDE; break;
                case '0': cmd = CMD_ZERO_ALTITUDE; break;
                case 'f':
                    cmd = CMD_SET_RATE_HZ;
                    arg = rate = nextRate(rate);
                    break;
                default:
                    cmd = CMD_SET_RATE_HZ;
                    arg = 30;   // não aceita pelo A: testa o REJECTED
                    break;
                }
                if (!commandSubmit(cmd, arg, CMD_FROM_CONSOLE))
                    Serial.printf("[cmd] tecla %c descartada: fila de comandos cheia\n", k);
                break;
            }
            case 'a':
                streamA = !streamA;
                Serial.printf("[B] fluxo de pacotes %s\n", streamA ? "ligado" : "desligado");
                break;
            case '\r':
            case '\n':
            case ' ': break;
            default: printHelp(); break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SERIAL_TASK_PERIOD_MS));
    }
}

bool serialTaskStart() {
    return xTaskCreatePinnedToCore(serialTask, "serial", SERIAL_TASK_STACK, nullptr, SERIAL_TASK_PRIO, nullptr,
                                   SERIAL_TASK_CORE) == pdPASS;
}
