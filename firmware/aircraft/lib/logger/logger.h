// logger.h — saída de texto do firmware por um ponto único (docs/refatoracao.md, R2).
// As mensagens vão para um buffer próprio, e o logPump() as passa para o Serial só no espaço livre do buffer
// de transmissão: quem imprime nunca espera a UART. Com o buffer cheio, a mensagem é descartada e contada; as
// seguintes também, até o buffer esvaziar e sair a linha "[log] … descartadas" (ordem: lib/log_line/log_core.h;
// pendência 2 do refatoracao.md).
// Os logs do núcleo (log_e…, via ets_printf) e do ESP-IDF (ESP_LOGx) também passam por aqui: sem isso, cada
// [E][Wire.cpp] esperava a transmissão inteira em curso (dezenas de ms).
// Na semana 3, o logPump() vai para a task da serial, e quem imprime não muda.
// O nome não é logf() porque logf() é o logaritmo do math.h.
#pragma once
#include <stddef.h>

// Cria o buffer (bytes) e redireciona os logs do núcleo e do IDF. Chamar logo depois do Serial.begin().
// Devolve false sem memória: as mensagens seguem direto para o Serial, como antes.
bool logBegin(size_t bytes);

void logPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logPrint(const char *s);
void logPrintln(const char *s = "");
// Passa o que couber para o Serial, sem esperar. Com o buffer vazio, publica também o aviso de descartes
// ainda não avisados (a última perda de uma rajada). Chamar a cada volta do laço
void logPump();
// Espera até haver bytes livres no buffer e nenhum descarte pendente, esvaziando-o (saídas longas, como o
// dump do d)
void logWaitRoom(size_t bytes);
// Espera tudo ser transmitido (antes de um reinício, ou depois de uma saída longa)
void logFlush();
