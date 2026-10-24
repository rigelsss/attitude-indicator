// http_static.h — partes puras do servidor HTTP próprio do ESP B (docs/web.md, passo 3): leitura da linha de
// requisição, caminho do arquivo, tipo e cabeçalho da resposta. Lógica pura (sem Arduino.h), testável com
// pio test -e native.
// O servidor (web_task) atende várias conexões ao mesmo tempo, sem travar: o WebServer do núcleo atendia uma por
// vez e ficava até 5 s preso numa conexão sem requisição (as conexões antecipadas dos navegadores de celular), e
// um segundo celular não conseguia abrir a página (bancada de 23/10/2026). Só arquivos estáticos (GET), sempre
// com Connection: close.
#pragma once
#include <stddef.h>
#include <stdint.h>

static const size_t HTTP_PATH_MAX = 64;

enum class HttpParse : uint8_t {
    NeedMore,   // a linha de requisição ainda não chegou inteira
    Ok,         // path preenchido
    Bad,        // linha malformada ou caminho inseguro (..)
    TooLong,    // buffer cheio sem o fim da linha
    NotGet,     // outro método (HEAD, POST...)
};

// buf: o que chegou até agora (não precisa terminar em \0); cap: tamanho do buffer de quem lê. O caminho sai
// normalizado: sem a query (?…), "/" vira "/index.html", e um caminho terminado em "/" ganha "index.html"
HttpParse httpParseRequest(const char *buf, size_t len, size_t cap, char *path, size_t pathLen);
const char *httpContentType(const char *path);
// Cabeçalho de uma resposta com corpo de bodyLen bytes. Devolve o tamanho escrito (0 se não coube)
size_t httpBuildHeader(int code, const char *contentType, size_t bodyLen, char *out, size_t n);
const char *httpReason(int code);
