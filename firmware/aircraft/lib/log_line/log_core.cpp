// log_core.cpp — ver log_core.h
#include "log_core.h"
#include <stdio.h>

// Maior aviso possível (dois uint32_t com 10 dígitos cabem com folga)
static const size_t NOTE_CAP = 96;

LogCore::Drops LogCore::take() {
    lock_.lock();
    const Drops d = {dropMsgs_, dropBytes_};
    dropMsgs_ = 0;
    dropBytes_ = 0;
    lock_.unlock();
    return d;
}

void LogCore::addDrop(uint32_t bytes) {
    lock_.lock();
    dropMsgs_++;
    dropBytes_ += bytes;
    lock_.unlock();
}

uint32_t LogCore::pendingMsgs() const {
    lock_.lock();
    const uint32_t n = dropMsgs_;
    lock_.unlock();
    return n;
}

size_t LogCore::formatNote(const Drops &d, char *out, size_t cap) {
    const int n = snprintf(out, cap, "[log] %lu mensagens descartadas (%lu bytes): buffer cheio\n",
                           (unsigned long)d.msgs, (unsigned long)d.bytes);
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

void LogCore::push(const char *s, size_t len) {
    if (len == 0) return;
    // Conferência e incremento na mesma seção: uma mensagem que começa depois de um descarte não passa
    // à frente do aviso dele
    lock_.lock();
    const bool holding = dropMsgs_ > 0;
    if (holding) {
        dropMsgs_++;
        dropBytes_ += (uint32_t)len;
    }
    lock_.unlock();
    if (holding) return;
    if (!buf_.send(s, len)) addDrop((uint32_t)len);
}

void LogCore::pump(LogSink &out) {
    for (;;) {
        const size_t room = out.room();
        if (room == 0) return;
        size_t n = 0;
        const char *p = buf_.acquire(room, n);
        if (p == nullptr) break;   // buffer vazio
        out.write(p, n);
        buf_.release(p);
    }
    // O espaço é conferido antes da reserva: depois dela, uma mensagem nova pode entrar no buffer, e o aviso
    // tem que sair antes dela, sem voltar ao contador. Só o consumidor escreve na saída, então o espaço não
    // diminui até a escrita
    if (out.room() < NOTE_CAP) return;
    const Drops d = take();
    if (d.msgs == 0) return;
    char note[NOTE_CAP];
    out.write(note, formatNote(d, note, sizeof(note)));
}
