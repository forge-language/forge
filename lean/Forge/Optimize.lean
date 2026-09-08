import Forge.AST
import Forge.Eval
import Forge.Typecheck

/-!
# Forge compile-time optimizer

Formal model of `fold_binary` / `simplify_binary` in `compiler/optimize.c`.
Algebraic rules apply only to well-typed int operations (see `Typecheck`).
-/

namespace Forge

open Expr BinOp Value

/-- Constant-fold integer binary operations (mirrors `fold_binary`). -/
def foldBinInt (op : BinOp) (a b : Int) : Option Expr :=
  match op with
  | .add => some (.intLit (a + b))
  | .sub => some (.intLit (a - b))
  | .mul => some (.intLit (a * b))
  | .div => if b = 0 then none else some (.intLit (a / b))
  | .mod => if b = 0 then none else some (.intLit (a % b))
  | .eq  => some (.boolLit (a = b))
  | .ne  => some (.boolLit (a ≠ b))
  | .lt  => some (.boolLit (a < b))
  | .le  => some (.boolLit (a ≤ b))
  | .gt  => some (.boolLit (a > b))
  | .ge  => some (.boolLit (a ≥ b))
  | .and | .or => none

/-- Constant-fold boolean binary operations. -/
def foldBinBool (op : BinOp) (a b : Bool) : Option Expr :=
  match op with
  | .eq  => some (.boolLit (a = b))
  | .ne  => some (.boolLit (a ≠ b))
  | .and => some (.boolLit (a && b))
  | .or  => some (.boolLit (a || b))
  | _    => none

/-- Constant folding on literal operands (`foldOrKeep` in `optimize.c`). -/
def foldOrKeep (op : BinOp) (l r : Expr) : Expr :=
  match l, r with
  | .intLit a, .intLit b =>
    match foldBinInt op a b with
    | some e => e
    | none   => .bin op l r
  | .boolLit a, .boolLit b =>
    match foldBinBool op a b with
    | some e => e
    | none   => .bin op l r
  | _, _ => .bin op l r

/-- Algebraic simplification then constant folding (`simplify_binary`). -/
def simplifyBinary (op : BinOp) (l r : Expr) : Expr :=
  if op = .add && isIntBin op l r then
    if l = .intLit 0 then r
    else if r = .intLit 0 then l
    else foldOrKeep op l r
  else if op = .sub && isIntBin op l r && r = .intLit 0 then l
  else if op = .mul && isIntBin op l r then
    if l = .intLit 1 then r
    else if r = .intLit 1 then l
    else foldOrKeep op l r
  else foldOrKeep op l r

/-- Bottom-up expression optimizer (`optimize_expr`). -/
def optimize : Expr → Expr
  | .intLit n  => .intLit n
  | .boolLit b => .boolLit b
  | .bin op l r =>
    let ol := optimize l
    let or_ := optimize r
    simplifyBinary op ol or_

end Forge
