// radio_commands.cpp — ver radio_commands.h
#include "radio_commands.h"
#include <math.h>
#include "config.h"
#include "logger.h"
#include "tasks/radio_task.h"

const char *commandName(uint8_t cmd) {
    switch (cmd) {
    case CMD_CALIBRATE_GYRO: return "calibrar giroscópio";
    case CMD_CALIBRATE_MAG:  return "calibrar magnetômetro";
    case CMD_ZERO_ATTITUDE:  return "zerar atitude";
    case CMD_ZERO_ALTITUDE:  return "zerar altitude";
    case CMD_SET_RATE_HZ:    return "mudar taxa";
    }
    return "desconhecido";
}

static const char *resultText(uint8_t r) {
    return r == ACK_DONE ? "DONE" : r == ACK_ACCEPTED ? "ACCEPTED" : "REJECTED";
}

static void sendAck(const CmdAck &a) {
    if (!radioSendAck(a)) logPrintf("[cmd] ACK de '%s' (seq %u) descartado: fila de ACKs cheia\n", commandName(a.cmd), a.cmdSeq);
}

void RadioCommands::reject(const SensorCmd &c, const char *why) {
    logPrintf("[cmd] '%s' (seq %u) recusado: %s\n", commandName(c.cmd), c.seq, why);
    sendAck(tracker_.finish(c.seq, c.cmd, ACK_REJECTED, millis()));
}

void RadioCommands::done(const SensorCmd &c) {
    logPrintf("[cmd] '%s' (seq %u) concluído\n", commandName(c.cmd), c.seq);
    sendAck(tracker_.finish(c.seq, c.cmd, ACK_DONE, millis()));
}

void RadioCommands::start(const SensorCmd &c, uint32_t timeoutMs) {
    logPrintf("[cmd] '%s' (seq %u) aceito e iniciado\n", commandName(c.cmd), c.seq);
    sendAck(tracker_.start(c.seq, c.cmd, millis(), timeoutMs));
}

void RadioCommands::complete(uint8_t result, const char *what) {
    const CmdAck a = tracker_.complete(result, millis());
    logPrintf("[cmd] '%s' (seq %u) %s: %s\n", commandName(a.cmd), a.cmdSeq, result == ACK_DONE ? "concluído" : "recusado",
              what);
    sendAck(a);
}

void RadioCommands::arrive(const SensorCmd &c) {
    CmdAck prev;
    switch (tracker_.arrive(c.seq, c.cmd, millis(), prev)) {
    case CmdArrival::Duplicate:
        // O B reenviou (não recebeu o ACK) ou reconsulta o comando aceito: devolve o resultado atual, sem executar
        // de novo. As reconsultas com o comando ainda em curso (ACCEPTED, a cada ~2 s) não são impressas
        if (prev.result != ACK_ACCEPTED)
            logPrintf("[cmd] '%s' (seq %u) repetido: reenviando %s\n", commandName(c.cmd), c.seq,
                      resultText(prev.result));
        sendAck(prev);
        return;
    case CmdArrival::Busy:
        reject(c, "outro comando do rádio em andamento");
        return;
    case CmdArrival::Full:
        // Sem guardar a recusa: o último lugar livre pode ser o reservado ao comando em curso
        logPrintf("[cmd] '%s' (seq %u) recusado: histórico de comandos cheio (resultados ainda retidos para o B)\n",
                  commandName(c.cmd), c.seq);
        sendAck(tracker_.refuse(c.seq, c.cmd));
        return;
    case CmdArrival::New:
        break;
    }

    switch (c.cmd) {
    case CMD_CALIBRATE_GYRO:
        // As mesmas regras do g: não interrompe as seis posições, a montagem nem a calibração do magnetômetro
        if ((cal_.mode() != CalMode::None && cal_.mode() != CalMode::GyroBoot) || mag_.calibrating()) {
            reject(c, "outra calibração em andamento");
            return;
        }
        gyroOutcomesAtStart_ = cal_.gyroCmdOutcomes();
        cal_.startGyro(GyroCalKind::Cmd);
        start(c, CMD_GYRO_TIMEOUT_MS);
        return;

    case CMD_CALIBRATE_MAG:
        if (!mag_.ok()) {
            reject(c, "magnetômetro indisponível");
            return;
        }
        if (mag_.calibrating() || !cal_.magAllowed()) {
            reject(c, "outra calibração em andamento");
            return;
        }
        mag_.startCalibration(false);
        if (!mag_.calibrating()) {
            reject(c, "a coleta não começou (ver as mensagens [mag])");
            return;
        }
        start(c, CMD_MAG_TIMEOUT_MS);
        return;

    case CMD_ZERO_ALTITUDE:
        if (!baro_.ok()) {
            reject(c, "barômetro indisponível");
            return;
        }
        if (baro_.zeroActive()) {
            reject(c, "zero da altitude já em andamento");
            return;
        }
        zeroAcceptsAtStart_ = baro_.zeroAccepts();
        baro_.zeroAltitude();
        if (!baro_.zeroActive()) {
            reject(c, "o zero não começou (ver as mensagens [baro])");
            return;
        }
        start(c, CMD_BARO_TIMEOUT_MS);
        return;

    case CMD_ZERO_ATTITUDE: {
        // O mesmo critério da telemetria: VAL_ATT inclui a idade da última amostra (com o INT parado, a atitude
        // continua "inicializada", mas não é atual)
        const uint16_t valid = telemetryValidFlags(validity_(), limits_);
        if (!(valid & VAL_ATT) || (valid & VAL_ATT_DEGRADED)) {
            reject(c, "atitude inválida, antiga ou degradada");
            return;
        }
        const float r = att_.rollDeg(), p = att_.pitchDeg();
        if (fabsf(r) > ZERO_ATT_MAX_DEG || fabsf(p) > ZERO_ATT_MAX_DEG) {
            logPrintf("[cmd] zero operacional: roll %+.2f°, pitch %+.2f° (limite ±%.0f°)\n", r, p, ZERO_ATT_MAX_DEG);
            reject(c, "inclinação acima do limite do zero operacional (use o z para a montagem)");
            return;
        }
        zeroRoll_ = r;
        zeroPitch_ = p;
        logPrintf("[cmd] zero operacional: roll %+.2f°, pitch %+.2f° subtraídos da telemetria até o próximo boot\n", r,
                  p);
        done(c);
        return;
    }

    case CMD_SET_RATE_HZ:
        if (!radioSetRateHz(c.arg)) {
            logPrintf("[cmd] taxa pedida: %ld Hz (aceitas: 10, 20, 25 e 50)\n", (long)c.arg);
            reject(c, "taxa inválida");
            return;
        }
        logPrintf("[cmd] taxa de envio: %ld Hz\n", (long)c.arg);
        done(c);
        return;

    default:
        reject(c, "comando desconhecido");
        return;
    }
}

void RadioCommands::service(uint32_t nowMs) {
    if (!tracker_.pending()) return;
    switch (tracker_.pendingCmd()) {
    case CMD_CALIBRATE_GYRO:
        if (cal_.gyroCmdOutcomes() != gyroOutcomesAtStart_) {
            if (cal_.gyroCmdAccepted()) complete(ACK_DONE, "bias aceito e gravado");
            else complete(ACK_REJECTED, "caixa não ficou parada (ver as mensagens [cal])");
            return;
        }
        break;
    case CMD_CALIBRATE_MAG:
        if (mag_.reviewing()) {
            switch (mag_.decideReview()) {
            case MagReview::Saved:      complete(ACK_DONE, "calibração gravada"); return;
            case MagReview::SaveFailed: complete(ACK_REJECTED, "falha ao gravar na NVS"); return;
            case MagReview::Rejected:   complete(ACK_REJECTED, "ajuste fora dos critérios (ver as mensagens [mag])"); return;
            case MagReview::None:       break;
            }
        } else if (!mag_.calibrating()) {
            complete(ACK_REJECTED, "coleta cancelada");
            return;
        }
        break;
    case CMD_ZERO_ALTITUDE:
        if (baro_.zeroAccepts() != zeroAcceptsAtStart_) {
            complete(ACK_DONE, "zero aceito");
            return;
        }
        break;
    default:
        break;
    }
    if (tracker_.timedOut(nowMs)) {
        // A coleta do magnetômetro é cancelada; a calibração do giroscópio e o zero continuam pela serial
        if (tracker_.pendingCmd() == CMD_CALIBRATE_MAG && mag_.calibrating()) mag_.handleKey('x');
        complete(ACK_REJECTED, "tempo esgotado");
    }
}
