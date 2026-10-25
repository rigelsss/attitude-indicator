// command_link.h — comandos do ESP B para o ESP A (docs/web.md, passo 2): seq, reenvio, casamento do ACK e
// tempos limite. Lógica pura (sem Arduino.h), testável com pio test -e native.
//   - cada comando recebe um seq (o primeiro é aleatório a cada boot, dado por quem cria: um B reiniciado não
//     repete os seq que o A lembra como já executados);
//   - enquanto não chega ACCEPTED, DONE ou REJECTED, o comando é reenviado a cada retryMs, até maxTries envios;
//     sem resposta depois do último, termina como NoResponse;
//   - ACCEPTED espera o DONE (ou REJECTED) por até completionMs; depois, NoCompletion. Nesse tempo, o comando é
//     reconsultado a cada pollMs com o mesmo seq: o A devolve o resultado atual sem executar de novo, e um DONE
//     perdido é recuperado na consulta seguinte (o A retém os resultados para isso);
//   - o ACK é casado pelo cmd_seq (e o comando); um ACK repetido, atrasado ou de seq desconhecido é ignorado;
//   - até SLOTS comandos ao mesmo tempo (o A recusa um segundo comando demorado com outro em curso).
#pragma once
#include <stdint.h>
#include "telemetry.h"

enum class CmdState : uint8_t {
    Free,
    Sending,        // enviado, sem ACK ainda (reenviando)
    Accepted,       // ACCEPTED: em execução no A
    Done,           // DONE
    Rejected,       // REJECTED
    NoResponse,     // nenhum ACK depois de maxTries envios
    NoCompletion,   // ACCEPTED sem DONE em completionMs
};
const char *cmdStateText(CmdState s);
inline bool cmdStateFinal(CmdState s) { return s != CmdState::Sending && s != CmdState::Accepted; }

struct CmdLinkConfig {
    uint32_t retryMs;
    uint8_t  maxTries;
    uint32_t completionMs;
    uint32_t pollMs;   // reconsulta do comando aceito
};

struct CmdSend {   // um envio (o primeiro ou um reenvio)
    uint8_t  cmd;
    int32_t  arg;
    uint16_t seq;
    uint8_t  attempt;   // 1 = primeiro envio; 0 = reconsulta de um comando aceito
    uint8_t  origin;    // quem pediu
    uint16_t rid;       // identificador do pedido dado pelo cliente do WebSocket (0 = nenhum)
};

struct CmdEvent {   // mudança de estado de um comando
    uint8_t  cmd;
    int32_t  arg;
    uint16_t seq;
    uint8_t  origin;   // quem pediu (definido por quem usa: console, encoder, página)
    CmdState state;
    uint16_t rid;      // identificador do pedido, repetido em todos os eventos dele (0 = nenhum)
};

// CommandPacket de um envio: cabeçalho MSG_COMMAND com o seq do comando, reservas em 0
CommandPacket buildCommandPacket(const CmdSend &s);
// Nome do comando, para a serial e a página
const char *commandText(uint8_t cmd);

class CommandLink {
public:
    static const int SLOTS = 4;

    CommandLink(const CmdLinkConfig &cfg, uint16_t firstSeq) : cfg_(cfg), nextSeq_(firstSeq) {}
    // Novo comando. false com todos os lugares ocupados por comandos em curso. rid: identificador do pedido, só
    // guardado e repetido nos eventos (o cliente casa o andamento com o pedido sem depender do seq)
    bool submit(uint8_t cmd, int32_t arg, uint8_t origin, uint32_t nowMs, uint16_t &seq, uint16_t rid = 0);
    // Próximo envio vencido (primeiro, reenvio ou reconsulta do aceito). false se nenhum
    bool nextSend(uint32_t nowMs, CmdSend &out);
    // Próximo comando que terminou por tempo (NoResponse ou NoCompletion). false se nenhum
    bool nextTimeout(uint32_t nowMs, CmdEvent &ev);
    // ACK recebido. true com mudança de estado (ev); false se desconhecido, repetido ou atrasado
    bool onAck(uint16_t cmdSeq, uint8_t cmd, uint8_t result, uint32_t nowMs, CmdEvent &ev);

private:
    struct Slot {
        CmdState state = CmdState::Free;
        uint8_t  cmd = 0, origin = 0, tries = 0;
        int32_t  arg = 0;
        uint16_t seq = 0, rid = 0;
        uint32_t nextMs = 0;       // Sending: próximo envio (ou fim, depois do último)
        uint32_t acceptedMs = 0;   // Accepted: instante do ACCEPTED
        uint32_t pollMs = 0;       // Accepted: próxima reconsulta
    };
    static CmdEvent event(const Slot &s);

    CmdLinkConfig cfg_;
    uint16_t nextSeq_;
    Slot slots_[SLOTS];
};
