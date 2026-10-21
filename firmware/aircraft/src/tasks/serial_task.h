// serial_task.h — task da serial, de baixa prioridade: inicia o Serial no próprio núcleo (a ISR da UART fica
// fora do núcleo da sensor_task), formata os relatórios do console (c, r, a; src/console_view.h), passa as
// mensagens do lib/logger para a UART e entrega as teclas à sensor_task pela fila de comandos.
// Núcleo SERIAL_TASK_CORE (0), prioridade SERIAL_TASK_PRIO (config.h).
#pragma once

// Cria a task. false sem memória
bool serialTaskStart();
