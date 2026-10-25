// tcp_conn.h — operações numa conexão TCP do lwip que a API de sockets não oferece (docs/web.md). As duas rodam na
// thread do TCP/IP (tcpip_api_call): só ela mexe nos pcbs, e o SDK do Arduino não usa a trava do núcleo do lwip.
#pragma once

// Encerra a conexão com reset (RST), descartando o que estava para enviar. O close() normal manda FIN depois dos
// dados pendentes e mantém a conexão (com os buffers, até ~5,7 kB) retransmitindo por muitos minutos a um cliente
// que sumiu e não responde: na bancada de 24/10/2026, cada PC derrubado levou 4 a 10 kB do heap que não voltaram.
// O SO_LINGER, que faria isso pelo caminho padrão, está desligado no SDK; o reset é feito com tcp_abort, como o
// lwip trata um reset recebido. Depois, o socket ainda precisa ser fechado (WiFiClient::stop).
// true se a conexão foi encerrada; false se ela já não existia (fechada ou com erro)
bool tcpReset(int fd);

// Bytes já entregues ao TCP e ainda sem confirmação do outro lado (não enviados + enviados sem ACK). −1 se a
// conexão já não existe. A web_task para de mandar a um cliente com mais que WS_MAX_INFLIGHT pendentes: um
// celular que some prende no máximo isso, em vez do buffer inteiro mais os buffers do Wi-Fi (~20 kB do heap com
// um celular em modo avião, na bancada de 24/10/2026)
int tcpInFlight(int fd);
