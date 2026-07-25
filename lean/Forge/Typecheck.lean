import Forge.AST

/-!
# Forge expression typing

Static types for the Forge expression subset used in optimizer proofs.
-/

namespace Forge

/-- Static types for expressions. -/
inductive Ty
  | int
  | bool
  deriving Repr, DecidableEq

/-- Static type of an expression (`none` = ill-typed). -/
def typeOf : Expr → Option Ty
  | .intLit _ => some .int
  | .boolLit _ => some .bool
  | .bin op l r =>
    match typeOf l, typeOf r with
    | some .int, some .int =>
      match op with
      | .add | .sub | .mul | .div | .mod => some .int
      | .eq | .ne | .lt | .le | .gt | .ge => some .bool
      | .and | .or => none
    | some .bool, some .bool =>
      match op with
      | .eq | .ne | .and | .or => some .bool
      | _ => none
    | _, _ => none

/-- The expression is a well-formed binary int operation. -/
def isIntBin (op : BinOp) (l r : Expr) : Bool :=
  typeOf (.bin op l r) == some .int

/-- If a binary expression is typed as `int`, both operands are `int`. -/
theorem int_types_of_bin_int (op : BinOp) (l r : Expr)
    (h : typeOf (.bin op l r) = some .int) :
    typeOf l = some .int ∧ typeOf r = some .int := by
  have hl : typeOf l = some .int := by
    match hl : typeOf l with
    | none =>
      have hnone : typeOf (.bin op l r) = none := by simp [typeOf, hl]
      rw [hnone] at h; cases h
    | some .bool =>
      match hr : typeOf r with
      | none =>
        have hnone : typeOf (.bin op l r) = none := by simp [typeOf, hl, hr]
        rw [hnone] at h; cases h
      | some .int =>
        have hnone : typeOf (.bin op l r) = none := by simp [typeOf, hl, hr]
        rw [hnone] at h; cases h
      | some .bool =>
        have h' : typeOf (.bin op l r) = some .bool ∨ typeOf (.bin op l r) = none := by
          simp [typeOf, hl, hr]
          cases op <;> simp
        rcases h' with hbool | hnone
        · rw [hbool] at h; cases h
        · rw [hnone] at h; cases h
    | some .int => rfl
  have hr : typeOf r = some .int := by
    match hr : typeOf r with
    | none =>
      have hnone : typeOf (.bin op l r) = none := by simp [typeOf, hl, hr]
      rw [hnone] at h; cases h
    | some .bool =>
      have hnone : typeOf (.bin op l r) = none := by simp [typeOf, hl, hr]
      rw [hnone] at h; cases h
    | some .int => rfl
  exact ⟨hl, hr⟩

/-- `isIntBin` implies both operands have type `int`. -/
theorem int_types_of_isIntBin (op : BinOp) (l r : Expr)
    (h : isIntBin op l r = true) :
    typeOf l = some .int ∧ typeOf r = some .int := by
  unfold isIntBin at h
  simpa [beq_iff_eq] using int_types_of_bin_int op l r (by simpa [beq_iff_eq] using h)

end Forge
