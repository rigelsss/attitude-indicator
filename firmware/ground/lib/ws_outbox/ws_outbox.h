// ws_outbox.h — filas do WebSocket do ESP B (docs/web.md, passo 3). Lógica pura (sem Arduino.h), testável com
// pio test -e native.
//   - OutRing: fila por cliente das mensagens que não podem se perder (hello, andamento dos comandos, recusas).
//     A web_task só envia a quem tem espaço; o que não coube espera aqui, e a telemetria (substituível pela
//     próxima) só sai com a fila vazia. O andamento de um comando já na fila é substituído no lugar
//     (replaceFirst: só o estado mais recente importa); cheia, a web_task refaz a fila a partir do RecentBySeq,
//     em vez de perder a mais antiga (que podia ser um "done").
//   - RecentBySeq: o último estado dos comandos mais recentes, por seq, para quem conecta (página recarregada ou
//     cliente derrubado por WS_STALL_MS): sem isso, um "done" perdido deixaria "aceito, em curso" para sempre.
#pragma once
#include <stddef.h>
#include <stdint.h>

template <typename T, size_t N>
class OutRing {
public:
    // false se a fila estava cheia e a mais antiga saiu
    bool push(const T &v) {
        bool kept = true;
        if (count_ == N) {
            pop();
            kept = false;
        }
        items_[(head_ + count_) % N] = v;
        count_++;
        return kept;
    }
    // Substitui no lugar o primeiro item com pred(item) verdadeiro; false se nenhum
    template <typename P>
    bool replaceFirst(P pred, const T &v) {
        for (size_t i = 0; i < count_; i++) {
            T &it = items_[(head_ + i) % N];
            if (pred(it)) {
                it = v;
                return true;
            }
        }
        return false;
    }
    bool peek(T &out) const {
        if (count_ == 0) return false;
        out = items_[head_];
        return true;
    }
    void pop() {
        if (count_ == 0) return;
        head_ = (head_ + 1) % N;
        count_--;
    }
    void clear() { head_ = count_ = 0; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

private:
    T items_[N];
    size_t head_ = 0, count_ = 0;
};

template <typename T, size_t N>
class RecentBySeq {
public:
    // Atualiza o estado do comando seq (no lugar dele) ou o acrescenta como o mais novo; cheia, o mais antigo sai
    void upsert(uint16_t seq, const T &v) {
        for (size_t i = 0; i < count_; i++)
            if (seqs_[at(i)] == seq) {
                items_[at(i)] = v;
                return;
            }
        if (count_ == N) {
            head_ = (head_ + 1) % N;
            count_--;
        }
        seqs_[at(count_)] = seq;
        items_[at(count_)] = v;
        count_++;
    }
    size_t size() const { return count_; }
    // i = 0: o mais antigo
    const T &get(size_t i) const { return items_[at(i)]; }

private:
    size_t at(size_t i) const { return (head_ + i) % N; }
    T items_[N];
    uint16_t seqs_[N];
    size_t head_ = 0, count_ = 0;
};
