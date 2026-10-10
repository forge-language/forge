#include "source.h"
#include <stdio.h>
#include <string.h>

static bool diagnostics_json;
void forge_set_diagnostics_json(bool enabled) { diagnostics_json = enabled; }

/* Decode only to count UTF-16 units. Invalid/truncated UTF-8 consumes one byte
 * deterministically; offsets always originate in the original source buffer. */
SourcePosition forge_source_position(const SourceFile *file, size_t offset) {
    SourcePosition p = {0};
    if (!file) return p;
    if (offset > file->length) offset = file->length;
    for (size_t i = 0; i < offset;) {
        unsigned char c = (unsigned char)file->text[i];
        if (c == '\n') { p.line++; p.character = 0; i++; continue; }
        if (c == '\r' && i + 1 < file->length && file->text[i + 1] == '\n') {
            i++; continue;
        }
        size_t n = c >= 0xc2 && c <= 0xdf ? 2 :
                   c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
        bool valid = n > 1 && i + n <= offset;
        for (size_t j = 1; valid && j < n; j++)
            valid = ((unsigned char)file->text[i+j] & 0xc0) == 0x80;
        if (valid && n == 3) {
            unsigned char d = (unsigned char)file->text[i+1];
            valid = !(c == 0xe0 && d < 0xa0) && !(c == 0xed && d >= 0xa0);
        }
        if (valid && n == 4) {
            unsigned char d = (unsigned char)file->text[i+1];
            valid = !(c == 0xf0 && d < 0x90) && !(c == 0xf4 && d >= 0x90);
        }
        p.character += valid && n == 4 ? 2 : 1;
        i += valid ? n : 1;
    }
    return p;
}
static void json_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 32) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
void forge_source_diagnostic(SourceSpan span, const char *phase,
                             const char *message, const char *legacy) {
    bool valid = span.file && span.start <= span.end && span.end <= span.file->length;
    SourcePosition start = {0}, end = {0};
    if (valid) {
        start = forge_source_position(span.file, span.start);
        end = forge_source_position(span.file, span.end);
    }
    if (diagnostics_json) {
        fputs("{\"source\":\"forge\",\"severity\":\"error\",\"phase\":", stderr);
        json_string(stderr, phase);
        fputs(",\"message\":", stderr); json_string(stderr, message);
        fputs(",\"file\":", stderr);
        if (valid) json_string(stderr, span.file->path); else fputs("null", stderr);
        fputs(",\"range\":", stderr);
        if (valid) fprintf(stderr, "{\"start\":{\"line\":%zu,\"character\":%zu},\"end\":{\"line\":%zu,\"character\":%zu}}", start.line, start.character, end.line, end.character);
        else fputs("null", stderr);
        fputs("}\n", stderr);
    } else {
        fprintf(stderr, "%s\n", legacy);
        if (valid) fprintf(stderr, "forge: location: %s:%zu:%zu-%zu:%zu\n", span.file->path,
                           start.line+1, start.character+1, end.line+1, end.character+1);
    }
}
