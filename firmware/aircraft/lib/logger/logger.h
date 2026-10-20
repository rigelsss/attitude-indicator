// logger.h — saída de texto do firmware por um ponto único (docs/refatoracao.md, R2).
// Hoje repassa para o Serial. Na semana 3, passa a enfileirar para a task da serial (baixa
// prioridade), e quem imprime não muda. O nome não é logf() porque logf() é o logaritmo do math.h.
#pragma once

void logPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logPrint(const char *s);
void logPrintln(const char *s = "");
// Espera a transmissão terminar (antes de um reinício, ou depois de uma saída longa)
void logFlush();
