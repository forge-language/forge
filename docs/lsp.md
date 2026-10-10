# Forge language servers

The native `forge-lsp` and TypeScript language server now live in
[forge-language/language-server](https://github.com/forge-language/language-server).
That repository owns build, install, configuration and protocol tests. Install a
Forge SDK first and configure the native server with `CMAKE_PREFIX_PATH`.

[VS Code / Cursor](https://github.com/forge-language/vscode-extension) and
[Vim / Neovim](https://github.com/forge-language/editor-configs) clients are separate
repositories. They use an installed `forge-lsp` on PATH or an explicit server path.

Compiler integration uses `forge <file> --check` and `--symbols-json`. The build SDK
root is `build/`, and an installed SDK uses its install prefix for headers/libraries.

The native Forge event loop checks edited buffers asynchronously on POSIX,
with version/configuration guards, cancellation and a shared four-child limit.
Both servers consume stage0's UTF-16 diagnostic ranges; imported-file errors
retain the original file in their messages. See the language-server repository
for protocol coverage, SDK requirements and platform limits.
