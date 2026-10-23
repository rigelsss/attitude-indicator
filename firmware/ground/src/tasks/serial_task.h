// serial_task.h — task da serial do ESP B, de baixa prioridade e única dona do Serial depois do boot: lê as
// teclas do console e imprime, a partir das cópias publicadas pela link_task, os eventos do link (sempre), o
// resumo de cada janela (r) e o fluxo de pacotes (a), além do estado (c).
// Núcleo SERIAL_TASK_CORE (0), prioridade SERIAL_TASK_PRIO (config.h).
#pragma once

// Cria a task. O Serial já deve estar iniciado (setup). false sem memória
bool serialTaskStart();
