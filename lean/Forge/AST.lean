/-!
# Forge abstract syntax

Formal model of the Forge compiler expression subset (`compiler/ast.h`, `compiler/optimize.c`).
-/

namespace Forge

/-- Binary operators in Forge (`BinOp` in `compiler/ast.h`). -/
inductive BinOp
  | add | sub | mul | div | mod
  | eq | ne | lt | le | gt | ge
  | and | or
  deriving Repr, DecidableEq

/-- Expression AST (integer / boolean literals and binary nodes). -/
inductive Expr
  | intLit (n : Int)
  | boolLit (b : Bool)
  | bin (op : BinOp) (l r : Expr)
  deriving Repr, DecidableEq

/-- Runtime values produced by evaluation. -/
inductive Value
  | int (n : Int)
  | bool (b : Bool)
  deriving Repr, DecidableEq

end Forge
