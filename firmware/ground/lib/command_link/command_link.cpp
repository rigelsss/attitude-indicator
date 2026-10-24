// command_link.cpp — ver command_link.h
#include "command_link.h"
#include <string.h>

const char *cmdStateText(CmdState s) {
    switch (s) {
    case CmdState::Free:         return "livre";
    case CmdState::Sending:      return "enviado";
    case CmdState::Accepted:     return "aceito";
    case CmdState::Done:         return "concluído";
    case CmdState::Rejected:     return "recusado";
    case CmdState::NoResponse:   return "sem resposta";
    case CmdState::NoCompletion: return "sem conclusão";
    }
    return "?";
}

CommandPacket buildCommandPacket(const CmdSend &s) {
    CommandPacket p;
    memset(&p, 0, sizeof(p));
    p.hdr.magic = TELEMETRY_MAGIC;
    p.hdr.version = TELEMETRY_VERSION;
    p.hdr.type = MSG_COMMAND;
    p.hdr.seq = s.seq;
    p.cmd = s.cmd;
    p.arg = s.arg;
    return p;
}

const char *commandText(uint8_t cmd) {
    switch (cmd) {
    case CMD_CALIBRATE_GYRO: return "calibrar giroscópio";
    case CMD_CALIBRATE_MAG:  return "calibrar magnetômetro";
    case CMD_ZERO_ATTITUDE:  return "zerar atitude";
    case CMD_ZERO_ALTITUDE:  return "zerar altitude";
    case CMD_SET_RATE_HZ:    return "mudar taxa";
    }
    return "desconhecido";
}

CmdEvent CommandLink::event(const Slot &s) { return {s.cmd, s.arg, s.seq, s.origin, s.state}; }

bool CommandLink::submit(uint8_t cmd, int32_t arg, uint8_t origin, uint32_t nowMs, uint16_t &seq) {
    for (Slot &s : slots_) {
        if (!cmdStateFinal(s.state) && s.state != CmdState::Free) continue;
        s.state = CmdState::Sending;
        s.cmd = cmd;
        s.arg = arg;
        s.origin = origin;
        s.tries = 0;
        s.seq = seq = nextSeq_++;
        s.nextMs = nowMs;
        return true;
    }
    return false;
}

bool CommandLink::nextSend(uint32_t nowMs, CmdSend &out) {
    for (Slot &s : slots_) {
        if (s.state == CmdState::Accepted && (int32_t)(nowMs - s.pollMs) >= 0) {
            s.pollMs = nowMs + cfg_.pollMs;
            out = {s.cmd, s.arg, s.seq, 0, s.origin};
            return true;
        }
        if (s.state != CmdState::Sending || s.tries >= cfg_.maxTries) continue;
        if ((int32_t)(nowMs - s.nextMs) < 0) continue;
        s.tries++;
        s.nextMs = nowMs + cfg_.retryMs;   // depois do último envio, é o prazo da resposta
        out = {s.cmd, s.arg, s.seq, s.tries, s.origin};
        return true;
    }
    return false;
}

bool CommandLink::nextTimeout(uint32_t nowMs, CmdEvent &ev) {
    for (Slot &s : slots_) {
        if (s.state == CmdState::Sending && s.tries >= cfg_.maxTries && (int32_t)(nowMs - s.nextMs) >= 0) {
            s.state = CmdState::NoResponse;
            ev = event(s);
            return true;
        }
        if (s.state == CmdState::Accepted && nowMs - s.acceptedMs >= cfg_.completionMs) {
            s.state = CmdState::NoCompletion;
            ev = event(s);
            return true;
        }
    }
    return false;
}

bool CommandLink::onAck(uint16_t cmdSeq, uint8_t cmd, uint8_t result, uint32_t nowMs, CmdEvent &ev) {
    for (Slot &s : slots_) {
        if (s.state == CmdState::Free || s.seq != cmdSeq || s.cmd != cmd) continue;
        if (cmdStateFinal(s.state)) return false;   // já terminou: ACK repetido ou atrasado
        CmdState next = s.state;
        if (result == ACK_ACCEPTED) {
            if (s.state != CmdState::Sending) return false;   // ACCEPTED repetido
            next = CmdState::Accepted;
            s.acceptedMs = nowMs;
            s.pollMs = nowMs + cfg_.pollMs;
        } else if (result == ACK_DONE) {
            next = CmdState::Done;
        } else if (result == ACK_REJECTED) {
            next = CmdState::Rejected;
        } else {
            return false;
        }
        s.state = next;
        ev = event(s);
        return true;
    }
    return false;
}
