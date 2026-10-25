// ws_frame.h — quadros de texto do WebSocket, do servidor (ESP B) ao cliente, enviados sem esperar (docs/web.md,
// passo 3). Lógica pura (sem Arduino.h), testável com pio test -e native.
// A web_task monta o quadro aqui e escreve direto no socket com MSG_DONTWAIT, sem a biblioteca: o sendTXT da
// biblioteca passa pelo WiFiClient::write, que espera até 1 s por tentativa, com até 10 tentativas (bancada de
// 24/10/2026: duas esperas de 1 s com o PC entrando e saindo da rede, e um celular ficou 5 s sem telemetria).
// Se só parte do quadro couber no buffer do TCP, o resto fica aqui para a próxima volta, e nada novo sai para
// aquele cliente antes disso (um quadro pela metade não pode ser intercalado com outro).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Cabeçalho de um quadro de texto do servidor (FIN, sem máscara: só o cliente mascara). Devolve 2 ou 4 bytes;
// 0 se len não cabe no tamanho estendido de 16 bits (as mensagens do B têm menos de 1 kB)
inline size_t wsTextHeader(size_t len, uint8_t *out) {
    if (len >= 65536) return 0;
    out[0] = 0x81;   // FIN + texto
    if (len < 126) {
        out[1] = (uint8_t)len;
        return 2;
    }
    out[1] = 126;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len & 0xFF);
    return 4;
}

// Um quadro por vez: load() monta, pending() diz o que falta, consumed() avança o que o socket aceitou
template <size_t N>
class FrameTx {
public:
    bool idle() const { return off_ == len_; }
    // Quadro começado e não terminado (o socket aceitou só parte)
    bool partial() const { return off_ > 0 && off_ < len_; }
    // false se ainda há quadro pendente ou se o quadro não cabe em N
    bool load(const char *msg, size_t n) {
        if (!idle()) return false;
        uint8_t head[4];
        const size_t h = wsTextHeader(n, head);
        if (h == 0 || h + n > N) return false;
        memcpy(buf_, head, h);
        memcpy(buf_ + h, msg, n);
        len_ = h + n;
        off_ = 0;
        return true;
    }
    const uint8_t *pending(size_t &n) const {
        n = len_ - off_;
        return buf_ + off_;
    }
    void consumed(size_t w) { off_ += w > len_ - off_ ? len_ - off_ : w; }
    void reset() { len_ = off_ = 0; }

private:
    uint8_t buf_[N];
    size_t len_ = 0, off_ = 0;
};
