# Source diagnostics

Stage0 reports the exact source range for parse and semantic errors. Ranges refer
to the original source buffer, including imported modules, rather than searching
for a repeated identifier in the source text.

Existing commands retain the first diagnostic line. When a source span exists, a
second line supplies its filename and range:

```text
forge: semantic: unknown value 'missing'
forge: location: example.fg:3:13-3:20
```

These positions are one-based lines and UTF-16 columns. The end is exclusive.
A filename can contain spaces or colons; consumers should read coordinates from
the right or use the structured format.

```sh
forge example.fg --check --diagnostics-json
```

This flag emits one JSON object per parse or semantic failure to stderr. Its
`range.start` and `range.end` use zero-based LSP positions. `file` identifies the
source that contains the error. When a synthetic AST has no source information,
`file` and `range` are `null`; a location is not invented. The command exits
nonzero on a failure. Successful checks produce no diagnostic object.

This format covers stage0 parse and semantic diagnostics. Driver, module-loading,
lexical and backend failures can still use legacy output. Stage2 does not yet
share this diagnostic implementation.

The TypeScript language server consumes the default location line while keeping
compatibility with older compilers. Imported-module locations appear in the
message rather than applying their coordinates to the open document.

The diagnostic regression suite covers raw multiline string tokens, repeated
names, qualified calls, imported and transitive declaration origins, CRLF,
Unicode UTF-16 columns, filename escaping, missing spans, and optimizer span
preservation.
