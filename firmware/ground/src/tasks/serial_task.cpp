// serial_task.cpp — ver serial_task.h
#include "serial_task.h"
#include <Arduino.h>
#include <esp_timer.h>
#include "config.h"
#include "alarm_task.h"
#include "input_task.h"
#include "link_task.h"
#include "radio_rx.h"

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
        Serial.printf("[enc] %lu ms: pressão longa (reservada para os comandos da semana 5)\n", (unsigned long)e.atMs);
        break;
    case InputEventMsg::AckDropped:
        Serial.printf("[enc] %lu ms: botão curto descartado: fila de reconhecimento do alarme cheia\n",
                      (unsigned long)e.atMs);
        break;
    }
}

static void printReport(const LinkReport &r) {
    const LinkCounts &w = r.win.counts;
    Serial.printf("[r] link %s | rec %lu em %lu ms | perdidos %lu | dup %lu | atrasados %lu | ressinc %lu | "
                  "recusados %lu | outro remetente %lu | fila cheia %lu",
                  linkStateText(r.state), (unsigned long)w.received, (unsigned long)r.windowMs, (unsigned long)w.lost,
                  (unsigned long)w.duplicates, (unsigned long)w.late, (unsigned long)w.resyncs,
                  (unsigned long)rxRefused(r.rxWin),
                  (unsigned long)r.rxWin.foreign, (unsigned long)r.rxWin.queueFull);
    if (r.win.intervals > 0)
        Serial.printf(" | intervalo %.1f/%.1f/%.1f ms (no A %lu/%lu ms)", r.win.intervalMinUs / 1000.0f,
                      (float)r.win.intervalSumUs / r.win.intervals / 1000.0f, r.win.intervalMaxUs / 1000.0f,
                      (unsigned long)r.win.srcIntervalMinMs, (unsigned long)r.win.srcIntervalMaxMs);
    else
        Serial.print(" | intervalo -");
    if (r.havePacket) Serial.printf(" | idade %lu ms | seq %u", (unsigned long)r.ageMs, r.lastSeq);
    Serial.printf(" | total: %lu rec, %lu perd (%.3f %%), %lu quedas\n", (unsigned long)r.totals.received,
                  (unsigned long)r.totals.lost, lossPct(r.totals), (unsigned long)r.totals.timeouts);
}

static void printPacket(const RxItem &it) {
    const TelemetryPacket &p = it.pkt;
    const uint64_t nowUs = (uint64_t)esp_timer_get_time();
    const unsigned long ageMs = nowUs > it.arrivalUs ? (unsigned long)((nowUs - it.arrivalUs) / 1000) : 0;
    Serial.printf("[a] seq %u | t %lu ms | roll %.2f pitch %.2f | proa %.1f | alt %.2f m | %.2f hPa | %.1f C | "
                  "status 0x%04X | valid 0x%04X | loop %u us | idade %lu ms\n",
                  p.hdr.seq, (unsigned long)p.t_ms, p.roll, p.pitch, p.heading, p.altitude_m, p.pressure_hpa,
                  p.temp_c, p.status, p.valid, p.loop_us, ageMs);
}

static void printState(bool streamR, bool streamA) {
    extern const char *const BUILD_STAMP;
    const RadioInfo &ri = radioInfo();
    Serial.println("---------------- ESP B (solo) ----------------");
    Serial.printf("firmware: compilado em %s | contrato v%d, TelemetryPacket %u bytes\n", BUILD_STAMP,
                  TELEMETRY_VERSION, (unsigned)sizeof(TelemetryPacket));
    if (ri.active)
        Serial.printf("rádio: ativo | MAC STA %s (recebe) | MAC AP %s | AP \"%s\" | canal %u | peer "
                      "%02X:%02X:%02X:%02X:%02X:%02X\n",
                      ri.staMac, ri.apMac, AP_SSID, ri.channel, PEER_MAC[0], PEER_MAC[1], PEER_MAC[2], PEER_MAC[3],
                      PEER_MAC[4], PEER_MAC[5]);
    else
        Serial.println("rádio: DESLIGADO (falha no boot; ver as linhas [radio])");
    Serial.printf("link: timeout %d ms, ressincroniza acima de %d pacotes, atrasado até %d pacotes e %d ms, resumo a "
                  "cada %d ms\n",
                  LINK_TIMEOUT_MS, LINK_RESYNC_GAP, LINK_LATE_MAX_PKTS, LINK_LATE_MAX_MS, LINK_REPORT_MS);
    LinkReport r;
    if (linkLatestReport(r)) {
        Serial.printf("estado (último resumo): %s", linkStateText(r.state));
        if (r.havePacket)
            Serial.printf(" | último seq %u, t_ms do A %lu, idade %lu ms", r.lastSeq, (unsigned long)r.lastTms,
                          (unsigned long)r.ageMs);
        Serial.println();
        Serial.printf("desde o boot: %lu rec | %lu perdidos (%.3f %%) | %lu dup | %lu atrasados | %lu ressinc | "
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
    Serial.printf("fluxos: r %s, a %s | heap livre %u kB\n", streamR ? "ligado" : "desligado",
                  streamA ? "ligado" : "desligado", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    Serial.println("----------------------------------------------");
}

static void printHelp() {
    Serial.println("[B] teclas: c = estado | r = resumo por segundo | a = fluxo de pacotes (10 Hz) | k = reconhecer o "
                   "alarme (o mesmo que o botão do encoder)");
}

static void serialTask(void *) {
    bool streamR = true;   // o resumo começa ligado: é o que a bancada do rádio acompanha
    bool streamA = false;
    uint32_t reportSeen = 0, droppedSeen = 0, alarmDroppedSeen = 0, inputDroppedSeen = 0;
    uint16_t lastSeqPrinted = 0;
    bool printedAny = false;
    TickType_t lastPkt = xTaskGetTickCount();
    for (;;) {
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
