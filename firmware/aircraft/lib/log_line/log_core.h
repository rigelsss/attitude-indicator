// log_core.h — núcleo do lib/logger: mensagens inteiras num buffer, descarte contado quando ele enche e
// aviso do descarte na ordem certa. O buffer, a saída e a trava entram por interfaces; no firmware são o
// esp_ringbuf, o Serial e um portMUX (lib/logger). Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Ordem do aviso "[log] … descartadas": ele sai depois de toda mensagem aceita antes do descarte e antes de
// toda mensagem que começou depois dele. Uma mensagem concorrente com o descarte pode sair de qualquer lado.
// Para isso:
//   - com um descarte pendente, os produtores descartam também as mensagens novas (só somam ao contador);
//   - só o consumidor (pump) publica o aviso, quando o buffer esvazia, e só ele escreve na saída.
// Custo: depois de um descarte, tudo é descartado até o buffer esvaziar (8 kB a 115200: até ~0,7 s).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Buffer de bytes com vários produtores e um consumidor
class LogBuffer {
public:
    // Guarda a mensagem inteira ou nada, sem esperar
    virtual bool send(const char *s, size_t len) = 0;
    virtual size_t freeBytes() const = 0;
    // Próximo trecho contíguo, com até max bytes (n recebe o tamanho); nullptr com o buffer vazio.
    // O trecho fica reservado até release()
    virtual const char *acquire(size_t max, size_t &n) = 0;
    virtual void release(const char *p) = 0;
    virtual ~LogBuffer() {}
};

// Destino das mensagens (a UART). Só o consumidor escreve nela, então o espaço livre só cresce entre
// uma consulta e a escrita seguinte
class LogSink {
public:
    virtual size_t room() = 0;
    virtual void write(const char *s, size_t len) = 0;
    virtual ~LogSink() {}
};

// Seção crítica curta em torno dos contadores de descarte (vale também em interrupção)
class LogLock {
public:
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual ~LogLock() {}
};

class LogCore {
public:
    LogCore(LogBuffer &buf, LogLock &lock) : buf_(buf), lock_(lock) {}
    // Uma mensagem de qualquer produtor, sem esperar. Com um descarte pendente, ela também é descartada
    void push(const char *s, size_t len);
    // Passa para a saída o que couber, sem esperar. Com o buffer vazio, publica o aviso dos descartes
    // pendentes. Só um consumidor
    void pump(LogSink &out);
    // Descartes ainda não avisados
    uint32_t pendingMsgs() const;

private:
    struct Drops {
        uint32_t msgs;
        uint32_t bytes;
    };
    // Reserva todo o pendente para o aviso; os produtores voltam a ser aceitos
    Drops take();
    void addDrop(uint32_t bytes);
    static size_t formatNote(const Drops &d, char *out, size_t cap);

    LogBuffer &buf_;
    LogLock &lock_;
    uint32_t dropMsgs_ = 0;    // protegidos por lock_, inclusive nas leituras
    uint32_t dropBytes_ = 0;
};
