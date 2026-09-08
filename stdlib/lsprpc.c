#include "forge/lsprpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_LSP_BUF_SIZE (1024 * 1024)
static char g_lsp_read_buf[FR_LSP_BUF_SIZE];

static int fr_starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s;
        char b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        s++;
        prefix++;
    }
    return 1;
}

const char *fr_lsp_read_message(void) {
    g_lsp_read_buf[0] = '\0';

    long content_length = -1;
    char line[4096];

    while (fgets(line, sizeof(line), stdin)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        if (l == 0) break;
        if (fr_starts_with_ci(line, "content-length:")) {
            content_length = atol(line + 15);
        }
    }

    if (content_length <= 0) {
        return g_lsp_read_buf;
    }
    if (content_length >= FR_LSP_BUF_SIZE) {
        content_length = FR_LSP_BUF_SIZE - 1;
    }

    size_t got = fread(g_lsp_read_buf, 1, (size_t)content_length, stdin);
    g_lsp_read_buf[got] = '\0';
    return g_lsp_read_buf;
}

void fr_lsp_write_message(const char *body) {
    if (!body) body = "";
    size_t len = strlen(body);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, stdout);
    fflush(stdout);
}
