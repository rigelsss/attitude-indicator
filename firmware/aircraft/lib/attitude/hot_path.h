// hot_path.h — HOT_PATH: põe uma função da lógica pura na IRAM no ESP32 (no native, não faz nada).
// Para o caminho por amostra da fusão: rodando da flash, o custo dele variou com a posição do código no binário
// (de ~30 para ~130 µs quando o binário cresceu com o Wi-Fi, 21/10/2026; docs/tasks.md, passo 4).
#pragma once
#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#define HOT_PATH IRAM_ATTR
#else
#define HOT_PATH
#endif
