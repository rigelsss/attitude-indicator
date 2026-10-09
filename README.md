# attitude-indicator — Horizonte Artificial com ESP32

Sistema de telemetria de atitude com dois ESP32: o **avião** (sensores + fusão) transmite via ESP-NOW para a **estação de solo** (TFT, OLED, alarmes), que publica os dados via Wi-Fi/WebSocket para o navegador e para um cliente Python.

```
ESP A (avião) ──ESP-NOW──▶ ESP B (solo) ──Wi-Fi AP / WebSocket──▶ Navegador + Python
```

| Pasta | Conteúdo |
|---|---|
| `firmware/shared` | Contrato de dados (`telemetry.h`) |
| `firmware/aircraft` | Firmware do ESP A |
| `firmware/ground` | Firmware do ESP B |
| `ground-station/web` | Página do horizonte (servida pelo ESP B) |
| `ground-station/python` | Cliente de log e gráficos |
| `docs` | Plano, protocolo, pinagem, relatório |

## Uso rápido
1. Gravar o ESP A: `cd firmware/aircraft && pio run -t upload`
2. Gravar o ESP B: `cd firmware/ground && pio run -t upload && pio run -t uploadfs`
3. Conectar no Wi-Fi `Horizonte-ESP` e abrir `http://192.168.4.1`
