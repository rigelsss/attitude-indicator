// pulse_test.h — teste de bancada dos pulsos falsos no INT (refatoracao.md, pendência 8), só no ambiente
// aircraft_pulsetest (-D PULSE_TEST): a tecla F injeta, por PULSE_TEST_MS, instantes falsos na fila do INT,
// pelo mesmo caminho da ISR, com intervalo aleatório entre PULSE_TEST_MIN_US e PULSE_TEST_MAX_US. Reproduz o
// fio do INT meio encaixado sem mexer em fio. O firmware de voo (ambiente aircraft) não tem esta tecla.
#pragma once
#ifdef PULSE_TEST

// Começa uma injeção (ignorado se uma já estiver em curso). Chamado pela sensor_task (tecla F)
void pulseTestStart();

#endif
