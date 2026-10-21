// console_view.h — formatação do console na serial_task: estado (c), resumo por segundo (r) e linha do fluxo de
// atitude (a), a partir das cópias da sensor_task (src/console_report.h). Não lê os serviços. Cada bloco é
// montado inteiro num buffer e entregue ao logger numa escrita só: linhas de outras tasks não caem no meio.
// Os textos são os mesmos de antes da semana 3 (docs/refatoracao.md, passo 8).
#pragma once
#include "report_queue.h"

// Formata e imprime um relatório recebido da fila. Só a serial_task chama (o buffer de formatação é dela)
void consoleViewPrint(const ReportMsg &msg);
