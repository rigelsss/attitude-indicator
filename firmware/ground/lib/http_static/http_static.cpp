// http_static.cpp — ver http_static.h
#include "http_static.h"
#include <stdio.h>
#include <string.h>

static bool endsWith(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

HttpParse httpParseRequest(const char *buf, size_t len, size_t cap, char *path, size_t pathLen) {
    if (pathLen > 0) path[0] = '\0';
    // Fim da linha de requisição
    size_t eol = 0;
    while (eol < len && buf[eol] != '\n') eol++;
    if (eol == len) return len >= cap ? HttpParse::TooLong : HttpParse::NeedMore;
    // "MÉTODO CAMINHO VERSÃO"
    const char *sp1 = (const char *)memchr(buf, ' ', eol);
    if (sp1 == nullptr) return HttpParse::Bad;
    const size_t mlen = (size_t)(sp1 - buf);
    const char *start = sp1 + 1;
    const char *sp2 = (const char *)memchr(start, ' ', eol - (size_t)(start - buf));
    if (sp2 == nullptr) return HttpParse::Bad;
    if (!(mlen == 3 && memcmp(buf, "GET", 3) == 0)) return HttpParse::NotGet;
    size_t plen = (size_t)(sp2 - start);
    const char *q = (const char *)memchr(start, '?', plen);
    if (q != nullptr) plen = (size_t)(q - start);
    if (plen == 0 || start[0] != '/') return HttpParse::Bad;
    const bool dir = start[plen - 1] == '/';
    if (plen + (dir ? strlen("index.html") : 0) + 1 > pathLen) return HttpParse::Bad;
    memcpy(path, start, plen);
    path[plen] = '\0';
    if (dir) strcat(path, "index.html");
    if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr) return HttpParse::Bad;   // fora da página
    return HttpParse::Ok;
}

const char *httpContentType(const char *path) {
    if (endsWith(path, ".html")) return "text/html; charset=utf-8";
    if (endsWith(path, ".css")) return "text/css; charset=utf-8";
    if (endsWith(path, ".js")) return "application/javascript; charset=utf-8";
    if (endsWith(path, ".json")) return "application/json";
    if (endsWith(path, ".svg")) return "image/svg+xml";
    if (endsWith(path, ".png")) return "image/png";
    if (endsWith(path, ".ico")) return "image/x-icon";
    return "text/plain; charset=utf-8";
}

const char *httpReason(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    case 503: return "Service Unavailable";
    }
    return "Error";
}

size_t httpBuildHeader(int code, const char *contentType, size_t bodyLen, char *out, size_t n) {
    // no-cache: depois de um uploadfs, o navegador busca a página nova
    const int w = snprintf(out, n,
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-cache\r\n"
                           "Connection: close\r\n\r\n",
                           code, httpReason(code), contentType, (unsigned)bodyLen);
    return (w > 0 && (size_t)w < n) ? (size_t)w : 0;
}
