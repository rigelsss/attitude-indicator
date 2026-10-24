// cmd_tracker.h — estado dos comandos do rádio no ESP A (docs/web.md, passo 1): duplicados, comando demorado em
// curso e tempo limite. Lógica pura (sem Arduino.h), testável com pio test -e native. Quem executa os comandos
// (src/radio_commands.cpp, na sensor_task) decide o resultado; aqui fica só a memória para os ACKs.
//   - o B reenvia o mesmo seq enquanto não recebe ACCEPTED ou REJECTED, e reconsulta o comando aceito até o
//     resultado final: um seq já visto (o comando em curso ou um resultado ainda retido) devolve o ACK atual
//     dele, sem executar de novo;
//   - cada resultado fica retido pelo tempo em que o B pode pedi-lo de novo, e não pela quantidade (um comando
//     cujo ACK se perdeu não volta a parecer novo porque outros terminaram): o de um comando demorado, por
//     longMs (o B o reconsulta enquanto espera o DONE); o imediato, inclusive as recusas, por shortMs (só os
//     reenvios do B, que acabam em ~1 s, o pedem de novo). Assim, uma tecla repetida não lota o histórico;
//   - capacidade: com CAPACITY resultados ainda retidos, um comando novo é recusado (Full) e não guardado, em
//     vez de apagar um resultado que ainda pode ser consultado. Com um comando demorado em curso, um lugar fica
//     reservado para o resultado dele;
//   - um comando demorado fica pendente de ACCEPTED até DONE ou REJECTED; outro comando nesse tempo é recusado
//     (Busy); o pendente expira em timeoutMs (o chamador responde REJECTED).
#pragma once
#include <stdint.h>

struct CmdAck {
    uint8_t  cmd;      // CommandId
    uint8_t  result;   // AckResult
    uint16_t cmdSeq;   // seq do CommandPacket
};

enum class CmdArrival : uint8_t {
    New,         // executar
    Duplicate,   // já visto: reenviar o ack devolvido, sem executar
    Busy,        // outro comando demorado em curso: recusar (finish com ACK_REJECTED)
    Full,        // sem lugar para guardar o resultado: recusar com refuse() (não guardado: não ocupa o lugar
                 // reservado ao comando em curso)
};

class CmdTracker {
public:
    static const int CAPACITY = 16;

    CmdTracker(uint32_t longMs, uint32_t shortMs) : longMs_(longMs), shortMs_(shortMs) {}
    CmdArrival arrive(uint16_t seq, uint8_t cmd, uint32_t nowMs, CmdAck &ack);
    // Resultado imediato (DONE ou REJECTED), guardado para os duplicados (New e Busy têm lugar garantido)
    CmdAck finish(uint16_t seq, uint8_t cmd, uint8_t result, uint32_t nowMs);
    // Recusa do Full: REJECTED sem guardar (o último lugar livre pode ser o reservado ao comando em curso). Um
    // reenvio do mesmo seq volta a ser avaliado, e com o histórico ainda cheio é recusado de novo
    CmdAck refuse(uint16_t seq, uint8_t cmd) const;
    // Começo de um comando demorado: ACCEPTED, pendente até complete() ou o tempo limite
    CmdAck start(uint16_t seq, uint8_t cmd, uint32_t nowMs, uint32_t timeoutMs);
    // Fim do pendente (DONE ou REJECTED), guardado no lugar reservado
    CmdAck complete(uint8_t result, uint32_t nowMs);

    bool pending() const { return pending_; }
    uint8_t pendingCmd() const { return pend_.cmd; }
    bool timedOut(uint32_t nowMs) const { return pending_ && nowMs - startMs_ >= timeoutMs_; }
    int retained() const;   // resultados retidos agora (sem o pendente)

private:
    struct Rec {
        bool     used;
        CmdAck   ack;
        uint32_t finishedMs;
        uint32_t keepMs;   // longMs (demorado) ou shortMs (imediato)
    };
    void expire(uint32_t nowMs);
    int freeSlots() const;
    void store(const CmdAck &a, uint32_t nowMs, uint32_t keepMs);

    uint32_t longMs_, shortMs_;
    bool     pending_ = false;
    CmdAck   pend_ = {0, 0, 0};
    uint32_t startMs_ = 0, timeoutMs_ = 0;
    Rec      recs_[CAPACITY] = {};
};
