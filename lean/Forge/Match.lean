/-!
# Forge integer pattern matching

Formal model of `match expr { pat => body, ... }` on integers (`examples/match.fg`).
Arms are tried top-to-bottom; `_` is a wildcard.
-/

namespace Forge

/-- Pattern in a `match` arm. -/
inductive Pat
  | lit (n : Int)
  | wild
  deriving Repr, DecidableEq

structure MatchArm where
  pat : Pat
  armId : Nat
  deriving Repr

/-- Does `pat` match value `v`? -/
def patMatches (pat : Pat) (v : Int) : Bool :=
  match pat with
  | .lit n => v = n
  | .wild   => true

/--
Select the first matching arm index (mirrors sequential arm dispatch in the compiler).
Returns `none` when no arm matches (empty arm list).
-/
def selectArm (v : Int) : List MatchArm → Option Nat
  | [] => none
  | a :: rest =>
    if patMatches a.pat v then some a.armId else selectArm v rest

/-- Example arms from `examples/match.fg` for HTTP status codes. -/
def httpArms : List MatchArm := [
  ⟨.lit 200, 0⟩,
  ⟨.lit 404, 1⟩,
  ⟨.wild, 2⟩
]

theorem selectArm_lit_match (n : Int) (rest : List MatchArm) :
    selectArm n (⟨.lit n, 0⟩ :: rest) = some 0 := by
  simp [selectArm, patMatches]

theorem selectArm_wild_catches (v : Int) :
    selectArm v [⟨.wild, 42⟩] = some 42 := by
  simp [selectArm, patMatches]

theorem selectArm_first_wins (v : Int) (arms : List MatchArm) (a : MatchArm)
    (h : patMatches a.pat v) :
    selectArm v (a :: arms) = some a.armId := by
  simp [selectArm, h]

theorem selectArm_skips_non_matching (v : Int) (a : MatchArm) (rest : List MatchArm) (k : Nat)
    (h : ¬ patMatches a.pat v) (hrest : selectArm v rest = some k) :
    selectArm v (a :: rest) = some k := by
  simp [selectArm, h, hrest]

theorem httpArms_200 : selectArm 200 httpArms = some 0 := by
  native_decide

theorem httpArms_404 : selectArm 404 httpArms = some 1 := by
  native_decide

theorem httpArms_other : selectArm 500 httpArms = some 2 := by
  native_decide

end Forge
