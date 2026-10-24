// Testes das partes puras do servidor HTTP próprio (lib/http_static).
// Rodar no PC: pio test -e native
#include <unity.h>
#include <string.h>
#include "http_static.h"

void setUp() {}
void tearDown() {}

static HttpParse parse(const char *req, char *path, size_t cap = 256) {
    return httpParseRequest(req, strlen(req), cap, path, HTTP_PATH_MAX);
}

static void test_root_and_files() {
    char p[HTTP_PATH_MAX];
    TEST_ASSERT_EQUAL(HttpParse::Ok, parse("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", p));
    TEST_ASSERT_EQUAL_STRING("/index.html", p);
    TEST_ASSERT_EQUAL(HttpParse::Ok, parse("GET /app.js HTTP/1.1\r\n", p));
    TEST_ASSERT_EQUAL_STRING("/app.js", p);
    TEST_ASSERT_EQUAL(HttpParse::Ok, parse("GET /style.css?v=2 HTTP/1.1\r\n", p));   // sem a query
    TEST_ASSERT_EQUAL_STRING("/style.css", p);
    TEST_ASSERT_EQUAL(HttpParse::Ok, parse("GET /docs/ HTTP/1.1\n", p));   // \n sozinho também
    TEST_ASSERT_EQUAL_STRING("/docs/index.html", p);
}

// A linha chega em pedaços: só decide com ela inteira
static void test_need_more_and_too_long() {
    char p[HTTP_PATH_MAX];
    TEST_ASSERT_EQUAL(HttpParse::NeedMore, parse("", p));
    TEST_ASSERT_EQUAL(HttpParse::NeedMore, parse("GET /index.ht", p));
    char big[40];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL(HttpParse::TooLong, httpParseRequest(big, sizeof(big), sizeof(big), p, sizeof(p)));
}

static void test_bad_and_not_get() {
    char p[HTTP_PATH_MAX];
    TEST_ASSERT_EQUAL(HttpParse::NotGet, parse("POST / HTTP/1.1\r\n", p));
    TEST_ASSERT_EQUAL(HttpParse::NotGet, parse("HEAD / HTTP/1.1\r\n", p));
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse("GET\r\n", p));
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse("GET /x\r\n", p));             // sem a versão
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse("GET x HTTP/1.1\r\n", p));     // não começa com /
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse("GET /../config HTTP/1.1\r\n", p));   // fora da página
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse("GET /a\\b HTTP/1.1\r\n", p));
    char longPath[120] = "GET /";
    for (int i = 0; i < 80; i++) strcat(longPath, "a");
    strcat(longPath, " HTTP/1.1\r\n");
    TEST_ASSERT_EQUAL(HttpParse::Bad, parse(longPath, p));   // caminho maior que HTTP_PATH_MAX
}

static void test_content_types() {
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", httpContentType("/index.html"));
    TEST_ASSERT_EQUAL_STRING("application/javascript; charset=utf-8", httpContentType("/app.js"));
    TEST_ASSERT_EQUAL_STRING("text/css; charset=utf-8", httpContentType("/style.css"));
    TEST_ASSERT_EQUAL_STRING("text/plain; charset=utf-8", httpContentType("/x.bin"));
}

static void test_header() {
    char h[256];
    const size_t n = httpBuildHeader(200, "text/html; charset=utf-8", 1234, h, sizeof(h));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL(0, strncmp(h, "HTTP/1.1 200 OK\r\n", 17));
    TEST_ASSERT_NOT_NULL(strstr(h, "Content-Length: 1234\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(h, "Connection: close\r\n"));
    TEST_ASSERT_EQUAL(0, strcmp(h + n - 4, "\r\n\r\n"));
    char small[20];
    TEST_ASSERT_EQUAL_size_t(0, httpBuildHeader(200, "text/html", 1, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("Not Found", httpReason(404));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_root_and_files);
    RUN_TEST(test_need_more_and_too_long);
    RUN_TEST(test_bad_and_not_get);
    RUN_TEST(test_content_types);
    RUN_TEST(test_header);
    return UNITY_END();
}
