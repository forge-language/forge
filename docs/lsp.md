# Forge Language Server (LSP)

Forge ships a self-hosted Language Server Protocol implementation — `forge-lsp` is itself
a native Forge program (`tools/forge-lsp/main.fg`), built alongside the compiler. It's used
by the VS Code/Cursor extension, Claude Code's built-in LSP tool, and the Neovim/Vim plugins
under `editors/`.

## Features

| Feature | Status |
|---------|--------|
| Syntax highlighting | `.fg` TextMate grammar / Vim syntax |
| Diagnostics | `forge --check` subprocess on edit |
| Completion | Keywords, types, snippets, stdlib, symbols |
| Hover | Keywords and stdlib functions |
| Document symbols | Outline from `forge --symbols-json` |

## Prerequisites

Build the compiler and native LSP binary:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/bin/forge-lsp`, a native binary speaking LSP over stdio — no Node.js
required to run it. (The `lsp/` TypeScript server still exists in the tree as reference but
is no longer used by any of the editor integrations below.)

## Install the VS Code / Cursor extension

From the repository root:

```bash
cmake --build build
cd editors/vscode
npm install
npm run build
```

### Option A — F5 (recommended for development)

1. Open the `forge` repository folder in VS Code or Cursor.
2. Run **Tasks: Run Build Task** (or let the pre-launch task build automatically).
3. Press **F5** and choose **Forge Extension**.
4. In the Extension Development Host window, open any `.fg` file.

### Option B — Install from folder

1. Build the extension as above.
2. Run **Extensions: Install Extension from Location…**
3. Select `editors/vscode/`.
4. Reload the window.

Workspace settings in `.vscode/settings.json` point `forge.path` at `build/bin/forge`.
The extension resolves the `forge-lsp` binary itself: `forge.lspPath` setting, then
`<workspace>/build/bin/forge-lsp`, then `PATH`.

## Run the language server standalone

```bash
./build/bin/forge-lsp
```

The server communicates over `Content-Length`-framed JSON-RPC on stdio (LSP default).

## Configuration

| Setting | Description |
|---------|-------------|
| `forge.path` | Compiler binary (default: `build/bin/forge`) |
| `forge.forgeRoot` | Project root passed as `--forge-root` |
| `forge.libDir` | Library directory (`--lib-dir`) |
| `forge.includePaths` | Extra `-I` paths for module search |
| `forge.lspPath` | Path to the `forge-lsp` binary (default: `build/bin/forge-lsp`, then `PATH`) |

Example `.vscode/settings.json`:

```json
{
  "forge.path": "${workspaceFolder}/build/bin/forge",
  "forge.forgeRoot": "${workspaceFolder}",
  "forge.libDir": "${workspaceFolder}/build/lib",
  "forge.includePaths": ["${workspaceFolder}/examples"]
}
```

## Compiler flags used by LSP

```bash
forge <file> --check          # parse + resolve modules; exit 0 on success
forge <file> --symbols-json # document symbols as JSON
```

## Architecture

```
editors/vscode/       VS Code / Cursor extension (client)
editors/nvim/         Neovim plugin (client)
editors/vim/          Vim8/vim-lsp plugin (client)
tools/forge-lsp/       Native Forge LSP server (main.fg), built to build/bin/forge-lsp
compiler/              C lexer/parser (invoked by forge-lsp via subprocess for diagnostics/symbols)
```

Future work: go-to-definition, references, formatting.
