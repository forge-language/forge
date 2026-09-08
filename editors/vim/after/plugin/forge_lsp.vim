" Registers forge-lsp with prabirshrestha/vim-lsp, if it's installed.
if exists('g:loaded_forge_lsp')
  finish
endif
let g:loaded_forge_lsp = 1

function! s:forge_root(start_dir) abort
  let l:dir = a:start_dir
  while 1
    if isdirectory(l:dir . '/.git') || filereadable(l:dir . '/CMakeLists.txt')
      return l:dir
    endif
    let l:parent = fnamemodify(l:dir, ':h')
    if l:parent ==# l:dir
      return a:start_dir
    endif
    let l:dir = l:parent
  endwhile
endfunction

function! s:forge_lsp_path() abort
  if exists('g:forge_lsp_path') && !empty(g:forge_lsp_path)
    return g:forge_lsp_path
  endif

  let l:root = s:forge_root(expand('%:p:h'))
  let l:candidate = l:root . '/build/bin/forge-lsp'
  if executable(l:candidate)
    return l:candidate
  endif

  return exepath('forge-lsp')
endfunction

function! s:register_forge_lsp() abort
  if !exists('*lsp#register_server')
    return
  endif

  call lsp#register_server({
      \ 'name': 'forge-lsp',
      \ 'cmd': {server_info -> [s:forge_lsp_path()]},
      \ 'root_uri': {server_info -> lsp#utils#path_to_uri(s:forge_root(getcwd()))},
      \ 'allowlist': ['forge'],
      \ })
endfunction

augroup forge_lsp_setup
  autocmd!
  autocmd User lsp_setup call s:register_forge_lsp()
augroup END

" vim-lsp may already have fired lsp_setup before this file loaded; register directly too.
call s:register_forge_lsp()
