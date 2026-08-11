#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "forge/docstore.h"
#include <stdlib.h>
#include <string.h>

#define FR_DOC_CAP 256

typedef struct {
    int used;
    char *uri;
    char *text;
} fr_doc_entry_t;

static fr_doc_entry_t g_docs[FR_DOC_CAP];

static int fr_doc_find(const char *uri) {
    for (int i = 0; i < FR_DOC_CAP; i++) {
        if (g_docs[i].used && strcmp(g_docs[i].uri, uri) == 0) return i;
    }
    return -1;
}

int64_t fr_doc_set(const char *uri, const char *text) {
    if (!uri) return 0;

    int idx = fr_doc_find(uri);
    if (idx < 0) {
        for (int i = 0; i < FR_DOC_CAP; i++) {
            if (!g_docs[i].used) { idx = i; break; }
        }
        if (idx < 0) return 0;
        g_docs[idx].uri = strdup(uri);
        g_docs[idx].text = NULL;
        g_docs[idx].used = 1;
    }

    free(g_docs[idx].text);
    g_docs[idx].text = strdup(text ? text : "");
    return 1;
}

const char *fr_doc_get(const char *uri) {
    int idx = fr_doc_find(uri);
    if (idx < 0) return "";
    return g_docs[idx].text ? g_docs[idx].text : "";
}

int64_t fr_doc_remove(const char *uri) {
    int idx = fr_doc_find(uri);
    if (idx < 0) return 0;
    free(g_docs[idx].uri);
    free(g_docs[idx].text);
    g_docs[idx].uri = NULL;
    g_docs[idx].text = NULL;
    g_docs[idx].used = 0;
    return 1;
}
