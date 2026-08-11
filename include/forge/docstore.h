#ifndef FORGE_DOCSTORE_H
#define FORGE_DOCSTORE_H

#include <stdint.h>

int64_t fr_doc_set(const char *uri, const char *text);
const char *fr_doc_get(const char *uri);
int64_t fr_doc_remove(const char *uri);

#endif
