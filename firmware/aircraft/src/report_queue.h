// report_queue.h — fila de relatórios do console, da sensor_task (que copia) para a serial_task (que formata).
// Uma fila só, para os relatórios saírem na ordem em que foram copiados. Cada item tem o tamanho do maior
// relatório (o c, ~0,6 kB); são REPORT_QUEUE_LEN itens. Envio sem espera: com a fila cheia, o relatório é
// descartado e contado.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "console_report.h"

static constexpr size_t maxSize(size_t a, size_t b) { return a > b ? a : b; }
static constexpr size_t REPORT_MAX_BYTES =
    maxSize(maxSize(sizeof(WindowReport), sizeof(AttReport)), sizeof(StatusReport));

struct ReportMsg {
    ReportKind kind;
    alignas(8) uint8_t data[REPORT_MAX_BYTES];
    // Cópia do relatório de volta para o tipo dele (o tipo certo é o de kind)
    template <typename T> T as() const {
        static_assert(sizeof(T) <= REPORT_MAX_BYTES, "relatório maior que o item da fila");
        T r;
        memcpy(&r, data, sizeof(T));
        return r;
    }
};

// Cria a fila (no boot, antes das tasks). false sem memória
bool reportQueueInit();
// Envia uma cópia, sem esperar. false com a fila cheia (o relatório é contado em reportDropped())
bool reportSendRaw(ReportKind kind, const void *r, size_t size);
template <typename T> bool reportSend(ReportKind kind, const T &r) {
    static_assert(sizeof(T) <= REPORT_MAX_BYTES, "relatório maior que o item da fila");
    return reportSendRaw(kind, &r, sizeof(T));
}
// Próximo relatório, sem esperar. false com a fila vazia
bool reportReceive(ReportMsg &out);
// Relatórios descartados com a fila cheia, desde o boot
uint32_t reportDropped();
