# attitude-indicator — Horizonte Artificial com ESP32

Projeto de telemetria de atitude com dois ESP32 NodeMCU-32S, desenvolvido com Arduino e PlatformIO para a disciplina de Sistemas Embarcados.

## Estado atual

O projeto está na etapa inicial. Os dois firmwares inicializam a serial a 115200 baud, imprimem o tamanho de `TelemetryPacket` e alternam um LED a cada 500 ms. O contrato binário compartilhado e as configurações de build já estão definidos.

Ainda não estão implementados: leitura de sensores, fusão de atitude, tasks FreeRTOS, comunicação ESP-NOW, displays, encoder, alarmes, AP Wi-Fi, HTTP e WebSocket. As pastas dos clientes web e Python contêm apenas `.gitkeep`. Os ambientes de teste `native` estão configurados, mas ainda não há casos de teste.

Na verificação de 09/10/2026, os ambientes `aircraft` e `ground` compilaram com sucesso. Isso valida a compilação da estrutura inicial; o funcionamento em hardware ainda precisa ser verificado.

## Arquitetura planejada

O **avião (ESP A)** fará a leitura do MPU6050, magnetômetro GY-273 e BME280, calculará a atitude e transmitirá os dados via ESP-NOW. A **estação de solo (ESP B)** exibirá o horizonte em TFT, dados em OLED e alarmes, além de disponibilizar telemetria e comandos para navegador e cliente Python via Wi-Fi/WebSocket.

```
ESP A (avião) ──ESP-NOW──▶ ESP B (solo) ──Wi-Fi AP / WebSocket──▶ Navegador + Python
```

| Pasta | Conteúdo |
|---|---|
| `firmware/shared` | Contrato de dados (`telemetry.h`) |
| `firmware/aircraft` | Firmware do ESP A |
| `firmware/ground` | Firmware do ESP B |
| `ground-station/web` | Reservada para a página do horizonte, a ser servida pelo ESP B |
| `ground-station/python` | Reservada para o cliente de log, gráficos e comandos |
| `docs` | Plano de desenvolvimento e proposta de protocolo WebSocket |

## Ambiente de desenvolvimento

1. Instale o VS Code e a extensão PlatformIO IDE.
2. No VS Code, use **File > Open Workspace from File...** e abra [attitude-indicator.code-workspace](attitude-indicator.code-workspace).
3. O workspace apresenta `aircraft (ESP A)`, `ground (ESP B)` e `repositório`. Os dois primeiros nomes apontam para as pastas existentes em `firmware/`; não são cópias dos projetos.

O caminho do compilador configurado para o IntelliSense pressupõe Windows e instalação do PlatformIO em `%USERPROFILE%/.platformio`. Ajuste esse caminho se o seu ambiente for diferente.

## Compilar e gravar os firmwares atuais

Use um terminal do PlatformIO no VS Code, onde o comando `pio` esteja disponível. Os comandos abaixo partem da raiz do repositório e também funcionam no PowerShell sem depender de `&&`.

Para o **ESP A**, execute em `firmware/aircraft`:

```powershell
cd firmware/aircraft
pio run -e aircraft
pio run -e aircraft -t upload
pio device monitor -b 115200
```

Para o **ESP B**, abra outro terminal na raiz e execute em `firmware/ground`:

```powershell
cd firmware/ground
pio run -e ground
pio run -e ground -t upload
pio device monitor -b 115200
```

Para apenas compilar, execute somente `pio run -e aircraft` ou `pio run -e ground` na pasta correspondente. Para gravar e monitorar, conecte a placa correspondente; se necessário, configure `upload_port` e `monitor_port` no respectivo `platformio.ini`.

O comportamento esperado do código atual é a mensagem de inicialização na serial e a alternância do LED definido em `include/config.h`: GPIO 2 no A e GPIO 27 no B. O LED do B precisa estar conectado conforme essa configuração.

### Diagnóstico I2C do ESP A

O ambiente `i2cscan` grava, no lugar do firmware normal, um diagnóstico que varre o barramento I2C, identifica os sensores e imprime as leituras brutas do MPU6050:

```powershell
cd firmware/aircraft
pio run -e i2cscan -t upload
pio device monitor -b 115200
```

O procedimento e os resultados observados ficam em [docs/hardware.md](docs/hardware.md).

## Fluxo futuro de Wi-Fi e página web

Após a implementação do AP, servidor HTTP/WebSocket e página web, o fluxo previsto será:

1. Na pasta `firmware/ground`, enviar os arquivos da página com `pio run -e ground -t uploadfs`.
2. Conectar à rede `Horizonte-ESP`.
3. Abrir `http://192.168.4.1` no navegador.

Esse fluxo ainda não está disponível. A configuração de LittleFS já aponta para `ground-station/web`, mas não há página nem servidor implementados.

## Próximos passos

A primeira demonstração integrada deve cobrir leitura do MPU, cálculo de roll/pitch, transmissão ESP-NOW e exibição na serial da estação de solo, incluindo detecção de perda do link.

Consulte o [plano de desenvolvimento](docs/plano.md) para as etapas e a [proposta de protocolo WebSocket](docs/protocolo-websocket.md) para o contrato previsto com os clientes.
