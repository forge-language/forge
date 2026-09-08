" Vim syntax file for Forge, ported from editors/vscode/syntaxes/forge.tmLanguage.json
if exists('b:current_syntax')
  finish
endif

syntax case match

syntax match forgeComment "//.*$"

syntax region forgeString start=/"/ skip=/\\./ end=/"/ contains=forgeEscape
syntax match forgeEscape "\\\\." contained

syntax match forgeNumber "\<[0-9]\+\(\.[0-9]\+\)\?\>"

syntax keyword forgeControl process coroutine supervisor spawn send recv yield await if else while for break continue return match on receive restart
syntax keyword forgeDeclaration let mut own move fn const struct enum native extern import library export
syntax keyword forgeBoolean true false
syntax keyword forgeType int float bool string void ptr

syntax match forgeOperator "\(+=\|-=\|\*=\|/=\|%=\|=>\|==\|!=\|<=\|>=\|&&\|||\|\.\|::\)"

highlight default link forgeComment Comment
highlight default link forgeString String
highlight default link forgeEscape SpecialChar
highlight default link forgeNumber Number
highlight default link forgeControl Statement
highlight default link forgeDeclaration Keyword
highlight default link forgeBoolean Boolean
highlight default link forgeType Type
highlight default link forgeOperator Operator

let b:current_syntax = 'forge'
