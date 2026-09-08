# Forge support for Neovim

Filetype detection, syntax highlighting, and native `forge-lsp` attachment for `.fg` files.

## Setup

Add this directory to your `runtimepath`, e.g. with a plugin manager pointed at the repo:

```lua
-- lazy.nvim
{ dir = '/path/to/forge/editors/nvim', name = 'forge.nvim' }
```

or manually:

```lua
vim.opt.runtimepath:append('/path/to/forge/editors/nvim')
```

## Requirements

Build the language server first:

```bash
cmake --build build --target forge-lsp
```

`ftplugin/forge.lua` looks for `forge-lsp` in this order:

1. `vim.g.forge_lsp_path`, if set
2. `build/bin/forge-lsp` walking up from the buffer's directory to the nearest `.git` or `CMakeLists.txt`
3. `forge-lsp` on `$PATH`

If none is found, the server simply isn't attached (no error).

## Verifying

Open an `.fg` file and run `:LspInfo` (requires `nvim-lspconfig` or Neovim's built-in `:checkhealth vim.lsp`) to confirm the `forge-lsp` client attached, then check hover (`K`) and diagnostics.
