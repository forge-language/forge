# Forge support for Vim8/Vim9 + vim-lsp

Filetype detection, syntax highlighting, and `forge-lsp` registration for `.fg` files, built on top of
[prabirshrestha/vim-lsp](https://github.com/prabirshrestha/vim-lsp) (an external dependency — install it
separately with your plugin manager).

## Setup

With a plugin manager (e.g. vim-plug):

```vim
Plug 'prabirshrestha/vim-lsp'
Plug '/path/to/forge/editors/vim'
```

or add the directory to `'runtimepath'` manually:

```vim
set runtimepath+=/path/to/forge/editors/vim
```

## Requirements

Build the language server first:

```bash
cmake --build build --target forge-lsp
```

`after/plugin/forge_lsp.vim` looks for `forge-lsp` in this order:

1. `g:forge_lsp_path`, if set
2. `build/bin/forge-lsp` under the nearest ancestor directory containing `.git` or `CMakeLists.txt`
3. `forge-lsp` on `$PATH`

If vim-lsp isn't installed (no `lsp#register_server`), this plugin is a no-op.

## Verifying

Open an `.fg` file and run `:LspStatus` to confirm the `forge-lsp` server registered, then check hover
(`:LspHover`) and diagnostics.
