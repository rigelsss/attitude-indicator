// logger.h — saída de texto do firmware por um ponto único (docs/refatoracao.md, R2).
// As mensagens vão para um buffer próprio, e o logPump() as passa para o Serial só no espaço livre do buffer
// de transmissão: quem imprime nunca espera a UART. Com o buffer cheio, a mensagem é descartada e contada; as
// seguintes também, até o buffer esvaziar e sair a linha "[log] … descartadas" (ordem: lib/log_line/log_core.h;
// pendência 2 do refatoracao.md).
// Os logs do núcleo (log_e…, via ets_printf) e do ESP-IDF (ESP_LOGx) também passam por aqui: sem isso, cada
// [E][Wire.cpp] esperava a transmissão inteira em curso (dezenas de ms).
// O nome não é logf() porque logf() é o logaritmo do math.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Cria o buffer (bytes) e redireciona os logs do núcleo e do IDF. Chamar no boot, antes de qualquer
// mensagem; até o logAttachSerial(), elas só se acumulam. flushTimeoutMs: espera máxima do logFlush() de
// outra task. Devolve false sem memória: modo degradado, sem buffer, em que as mensagens anteriores ao
// Serial pronto são descartadas e as seguintes são escritas direto, bloqueando quem imprime (como antes da P2).
bool logBegin(size_t bytes, uint32_t flushTimeoutMs);
// Chamado pela task da serial, logo depois do Serial.begin(): ela passa a ser a única que esvazia o buffer
// (logPump) e escreve no Serial, e o Serial é dado como pronto
void logAttachSerial();
// Espera o logAttachSerial() por até timeoutMs. false se a task da serial não sinalizou a tempo
bool logWaitSerial(uint32_t timeoutMs);

void logPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logPrint(const char *s);
void logPrintln(const char *s = "");
// Passa o que couber para o Serial, sem esperar. Com o buffer vazio, publica também o aviso de descartes
// ainda não avisados (a última perda de uma rajada). Só tem efeito na task registrada por logAttachSerial()
void logPump();
// Espera até haver bytes livres no buffer e nenhum descarte pendente, esvaziando-o (saídas longas, como o
// dump do d)
void logWaitRoom(size_t bytes);
// Espera tudo ser transmitido (antes de um reinício, ou depois de uma saída longa). De outra task, pede o flush
// à task da serial e espera a confirmação dela (até flushTimeoutMs): só ela sabe quando a escrita terminou
void logFlush();
