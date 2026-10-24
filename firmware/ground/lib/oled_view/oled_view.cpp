// oled_view.cpp — ver oled_view.h
#include "oled_view.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *NA = "---";

float oledHeading(float magneticDeg, bool headingTrue, float declinationDeg) {
    float h = headingTrue ? magneticDeg + declinationDeg : magneticDeg;
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (h >= 360.0f) h -= 360.0f;   // fmodf de um valor negativo pequeno pode dar 360 depois da soma
    return h;
}

// Valor com sinal e casas fixas, sem "-0.0" (um valor que arredonda para zero sai sem sinal negativo)
static void fmtSigned(char *buf, int n, float v, int decimals) {
    const float scale = decimals == 2 ? 100.0f : 10.0f;
    if (fabsf(v) * scale < 0.5f) v = 0.0f;
    snprintf(buf, n, decimals == 2 ? "%+.2f" : "%+.1f", (double)v);
}

static void setLine(OledPage &out, int row, const char *text) {
    snprintf(out.lines[row], sizeof(out.lines[row]), "%s", text);   // corta em OLED_COLS
}

// Título à esquerda e "p/N" na coluna da direita
static void title(OledPage &out, const char *text, const OledInputs &in) {
    char pg[8];
    snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(in.page + 1), (unsigned)in.pages);
    char line[OLED_COLS + 1];
    const int pgLen = (int)strlen(pg);
    snprintf(line, sizeof(line), "%-*.*s%s", OLED_COLS - pgLen, OLED_COLS - pgLen, text, pg);
    setLine(out, 0, line);
}

// Título da faixa sem dado atual
static const char *noDataTitle(const OledInputs &in) {
    if (!in.everLinked) return "SEM SINAL";
    if (!in.linkOk) return "LINK PERDIDO";
    return "DADO ATRASADO";
}

static void buildData(const OledInputs &in, OledPage &out) {
    const TelemetryPacket &p = in.pkt;
    const bool have = in.everLinked && in.linkOk && !in.dataLate;   // há um dado atual
    const bool att = have && (p.valid & VAL_ATT) && isfinite(p.roll) && isfinite(p.pitch);
    char a[12], b[12], line[OLED_COLS + 8];

    // Atitude degradada (giroscópio saturou, correção se recuperando): no título, para não parecer a normal
    const bool degraded = att && (p.valid & VAL_ATT_DEGRADED);
    title(out, have ? (degraded ? "ATT DEGRADADA" : "DADOS") : noDataTitle(in), in);
    out.banner = !have;

    if (att) {
        fmtSigned(a, sizeof(a), p.roll, 1);
        fmtSigned(b, sizeof(b), p.pitch, 1);
    } else {
        snprintf(a, sizeof(a), "%s", NA);
        snprintf(b, sizeof(b), "%s", NA);
    }
    snprintf(line, sizeof(line), "ROLL%6s PIT%6s", a, b);
    setLine(out, 1, line);

    const char *ref = in.headingTrue ? "VERD" : "MAG";
    if (have && (p.valid & VAL_HDG) && isfinite(p.heading)) {
        // Arredonda para o grau e mostra 000..359 (359,6 vira 000, e não 360)
        int h = (int)lroundf(oledHeading(p.heading, in.headingTrue, in.declinationDeg)) % 360;
        const bool gyroOnly = (p.valid & VAL_HDG_GYRO_ONLY) != 0;
        snprintf(line, sizeof(line), "PROA %03d%c%s", h, gyroOnly ? '~' : ' ', ref);
    } else {
        snprintf(line, sizeof(line), "PROA %s %s", NA, ref);
    }
    setLine(out, 2, line);

    if (have && (p.valid & VAL_ALT) && isfinite(p.altitude_m)) {
        fmtSigned(a, sizeof(a), p.altitude_m, 2);
        snprintf(line, sizeof(line), "ALT  %s m", a);
    } else {
        snprintf(line, sizeof(line), "ALT  %s m", NA);
    }
    setLine(out, 3, line);

    // VAL_ALT, e não só ST_BARO_OK (saúde do sensor): ela inclui a idade da leitura, e uma pressão parada não
    // fica na tela. Antes do zero da altitude (~4 s depois do boot do A), pressão e temperatura ficam em "---"
    if (have && (p.valid & VAL_ALT) && isfinite(p.pressure_hpa) && isfinite(p.temp_c))
        snprintf(line, sizeof(line), "%.1fhPa  %.1fC", (double)p.pressure_hpa, (double)p.temp_c);
    else
        snprintf(line, sizeof(line), "%shPa  %sC", NA, NA);
    setLine(out, 4, line);

    if (att && isfinite(p.gz)) {
        fmtSigned(a, sizeof(a), p.gz, 1);
        snprintf(line, sizeof(line), "CURVA %6s g/s", a);
    } else {
        snprintf(line, sizeof(line), "CURVA %6s g/s", NA);
    }
    setLine(out, 5, line);
}

static const char *okText(bool have, bool ok) { return !have ? "?" : (ok ? "ok" : "--"); }

static void buildLink(const OledInputs &in, OledPage &out) {
    const TelemetryPacket &p = in.pkt;
    const bool have = in.everLinked && in.linkOk && !in.dataLate;
    char line[OLED_COLS + 8];

    if (have && in.link.valid) snprintf(line, sizeof(line), "LINK OK %lu/s", (unsigned long)in.link.recPerS);
    else snprintf(line, sizeof(line), "%s", have ? "LINK OK" : noDataTitle(in));
    title(out, line, in);
    out.banner = !have;

    // Compacto para caber com os valores da bancada (1 049 perdidos, 17 %, 11 quedas: "perd 1049 17.0% q11")
    if (in.link.valid)
        snprintf(line, sizeof(line), in.link.lossPct < 10.0f ? "perd %lu %.2f%% q%lu" : "perd %lu %.1f%% q%lu",
                 (unsigned long)in.link.lostTotal, (double)in.link.lossPct, (unsigned long)in.link.timeouts);
    else
        snprintf(line, sizeof(line), "perd %s", NA);
    setLine(out, 1, line);

    // Intervalo entre chegadas (mín/médio/máx da janela) e atraso estimado do último pacote ("at", ms)
    // O atraso sempre cabe inteiro: se mín/médio/máx não couberem, o intervalo vira só mín-máx (cada valor
    // limitado a 9999, o pior caso "int 9999-9999 at 9999" tem 21 colunas)
    char late[8], ints[24];
    if (in.everLinked) snprintf(late, sizeof(late), "%lu", (unsigned long)(in.delayMs > 9999 ? 9999 : in.delayMs));
    else snprintf(late, sizeof(late), "%s", NA);
    const int room = OLED_COLS - (int)strlen("int  at ") - (int)strlen(late);
    if (in.link.valid && in.link.haveIntervals) {
        const float mn = fminf(in.link.intMinMs, 9999.0f), av = fminf(in.link.intAvgMs, 9999.0f),
                    mx = fminf(in.link.intMaxMs, 9999.0f);
        snprintf(ints, sizeof(ints), "%.0f/%.0f/%.0f", (double)mn, (double)av, (double)mx);
        if ((int)strlen(ints) > room) snprintf(ints, sizeof(ints), "%.0f-%.0f", (double)mn, (double)mx);
    } else {
        snprintf(ints, sizeof(ints), "%s", NA);
    }
    snprintf(line, sizeof(line), "int %s at %s", ints, late);
    setLine(out, 2, line);

    // Saúde do A: do último pacote, mesmo antigo ("?" antes do primeiro)
    const bool any = in.everLinked;
    snprintf(line, sizeof(line), "MPU %s BAR %s MAG %s", okText(any, p.status & ST_MPU_OK),
             okText(any, p.status & ST_BARO_OK), okText(any, p.status & ST_MAG_OK));
    setLine(out, 3, line);

    if (any)
        // "SIM" e não "EM CURSO": com loop de 5 dígitos, a linha precisa caber em 21 colunas
        snprintf(line, sizeof(line), "cal: %s loop %uus", (p.status & ST_CALIBRATING) ? "SIM" : "-",
                 (unsigned)p.loop_us);
    else
        snprintf(line, sizeof(line), "cal: %s", NA);
    setLine(out, 4, line);

    snprintf(line, sizeof(line), "ALARME %s", in.alarmText != nullptr ? in.alarmText : NA);
    setLine(out, 5, line);
}

void oledBuildPage(const OledInputs &in, OledPage &out) {
    memset(&out, 0, sizeof(out));
    if (in.page == 0) buildData(in, out);
    else buildLink(in, out);
    if (in.footer != nullptr) setLine(out, OLED_ROWS - 1, in.footer);
}

static const char *zeroLabel(uint8_t cmd) { return cmd == CMD_ZERO_ATTITUDE ? "ZERO ATT" : "ZERO ALT"; }

const char *oledEncoderFooter(char *buf, int n, bool buttonHeld, uint8_t pageCmd, bool haveEvent, uint8_t evCmd,
                              CmdState state, bool localFull, uint32_t ageMs, uint32_t showMs) {
    if (buttonHeld) {
        snprintf(buf, n, "segure 1s: %s", zeroLabel(pageCmd));
        return buf;
    }
    if (!haveEvent) return nullptr;
    const bool inProgress = !localFull && !cmdStateFinal(state);
    if (!inProgress && ageMs >= showMs) return nullptr;
    const char *txt = "?";
    if (localFull) {
        txt = "OCUPADO";
    } else {
        switch (state) {
        case CmdState::Sending:      txt = "ENVIADO"; break;
        case CmdState::Accepted:     txt = "ACEITO"; break;
        case CmdState::Done:         txt = "OK"; break;
        case CmdState::Rejected:     txt = "RECUSADO"; break;
        case CmdState::NoResponse:   txt = "SEM RESP."; break;
        case CmdState::NoCompletion: txt = "SEM FIM"; break;
        case CmdState::Free:         return nullptr;
        }
    }
    snprintf(buf, n, "%s: %s", zeroLabel(evCmd), txt);
    return buf;
}
