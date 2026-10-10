#ifndef FORGE_SOURCE_H
#define FORGE_SOURCE_H
#include <stddef.h>
#include <stdbool.h>

typedef struct SourceFile {
    char *path;
    const char *text; /* Borrowed; kept alive by entry caller or FileModule. */
    size_t length;
} SourceFile;
typedef struct SourceSpan {
    SourceFile *file;
    size_t start, end; /* Raw byte offsets, end exclusive. */
} SourceSpan;
typedef struct SourcePosition { size_t line, character; } SourcePosition;

void forge_set_diagnostics_json(bool enabled);
SourcePosition forge_source_position(const SourceFile *file, size_t offset);
/* Both message and legacy line exclude their trailing newline. */
void forge_source_diagnostic(SourceSpan span, const char *phase,
                             const char *message, const char *legacy);
#endif
