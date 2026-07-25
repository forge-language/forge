import Forge.Optimize

/-!
# Optimizer correctness

Shows that `optimize` is semantics-preserving w.r.t. `eval`.
-/

namespace Forge

open Expr BinOp Value Ty

private theorem eval_not_bool_of_int (e : Expr) (ht : typeOf e = some .int)
    (h : ∃ b, eval e = some (.bool b)) : False := by
  induction e with
  | intLit n =>
    rcases h with ⟨b, hb⟩
    simp [eval] at hb
  | boolLit b' =>
    rcases h with ⟨b, hb⟩
    simp [typeOf] at ht
  | bin op l r ih_l ih_r =>
    rcases h with ⟨b, hb⟩
    have ⟨hlty, hrty⟩ := int_types_of_bin_int op l r ht
    simp only [eval, bind, Option.bind] at hb
    rcases hl : eval l with ⟨⟩ | ⟨vl⟩
    · simp [hl] at hb
    rcases hr : eval r with ⟨⟩ | ⟨vr⟩
    · simp [hl, hr] at hb
    rcases vl with ⟨_⟩ | ⟨ba⟩
    · rcases vr with ⟨_⟩ | ⟨bb⟩
      · cases op <;> simp [evalBinInt, hl, hr, typeOf, hlty, hrty] at hb ht <;> try cases hb <;> try contradiction
      · have hex : ∃ b, eval r = some (.bool b) := ⟨bb, hr⟩
        exact ih_r hrty hex
    · have hex : ∃ b, eval l = some (.bool b) := ⟨ba, hl⟩
      exact ih_l hlty hex

private theorem foldBinInt_eval' (op : BinOp) (a b : Int) :
    eval (.bin op (.intLit a) (.intLit b)) =
      (foldBinInt op a b).bind eval := by
  cases op <;> simp [eval, evalBinInt, foldBinInt, Option.bind]
  · by_cases hb : b = 0 <;> simp [hb, eval]
  · by_cases hb : b = 0 <;> simp [hb, eval]

private theorem foldBinBool_eval' (op : BinOp) (a b : Bool) :
    eval (.bin op (.boolLit a) (.boolLit b)) =
      (foldBinBool op a b).bind eval := by
  cases op <;> simp [eval, evalBinBool, foldBinBool, Option.bind]
  <;> try (split <;> simp)

private theorem foldBinInt_eval_none (op : BinOp) (a b : Int)
    (h : foldBinInt op a b = none) : evalBinInt op a b = none := by
  cases op <;> simp [evalBinInt, foldBinInt, h] at h ⊢ <;> try cases h <;> rfl

private theorem foldBinBool_eval_none (op : BinOp) (a b : Bool)
    (h : foldBinBool op a b = none) : evalBinBool op a b = none := by
  cases op <;> simp [evalBinBool, foldBinBool, h] at h ⊢ <;> try cases h <;> rfl

private theorem foldBinInt_eval_some (op : BinOp) (a b : Int) (e : Expr)
    (h : foldBinInt op a b = some e) : eval e = evalBinInt op a b := by
  have h' := foldBinInt_eval' op a b
  rw [h, Option.bind] at h'
  exact h'.symm

private theorem foldBinBool_eval_some (op : BinOp) (a b : Bool) (e : Expr)
    (h : foldBinBool op a b = some e) : eval e = evalBinBool op a b := by
  have h' := foldBinBool_eval' op a b
  rw [h, Option.bind] at h'
  exact h'.symm

private theorem foldOrKeep_intLit_intLit (op : BinOp) (a b : Int) :
    eval (foldOrKeep op (.intLit a) (.intLit b)) = evalBinInt op a b := by
  unfold foldOrKeep
  match he : foldBinInt op a b with
  | some e =>
    simp [he]
    exact foldBinInt_eval_some op a b e he
  | none =>
    have h₁ := foldBinInt_eval' op a b
    have h₂ := foldBinInt_eval_none op a b he
    simp [he, h₂, Option.bind, foldOrKeep] at h₁ ⊢
    exact h₁

private theorem foldOrKeep_boolLit_boolLit (op : BinOp) (a b : Bool) :
    eval (foldOrKeep op (.boolLit a) (.boolLit b)) = evalBinBool op a b := by
  unfold foldOrKeep
  match he : foldBinBool op a b with
  | some e =>
    simp [he]
    exact foldBinBool_eval_some op a b e he
  | none =>
    have h₁ := foldBinBool_eval' op a b
    have h₂ := foldBinBool_eval_none op a b he
    simp [he, h₂, Option.bind, foldOrKeep] at h₁ ⊢
    exact h₁

private theorem foldOrKeep_eq_bin (op : BinOp) (l r : Expr)
    (h₁ : ¬∃ a b, l = .intLit a ∧ r = .intLit b)
    (h₂ : ¬∃ a b, l = .boolLit a ∧ r = .boolLit b) :
    foldOrKeep op l r = .bin op l r := by
  cases l with
  | intLit a =>
    cases r with
    | intLit b => exact absurd ⟨a, b, rfl, rfl⟩ h₁
    | boolLit _ | bin _ _ _ => rfl
  | boolLit ba =>
    cases r with
    | intLit _ => rfl
    | boolLit bb => exact absurd ⟨ba, bb, rfl, rfl⟩ h₂
    | bin _ _ _ => rfl
  | bin op' l' r' =>
    cases r with
    | intLit _ | boolLit _ | bin _ _ _ => rfl

theorem foldOrKeep_preserves (op : BinOp) (l r : Expr) :
    eval (foldOrKeep op l r) = eval (.bin op l r) := by
  by_cases h₁ : ∃ a b, l = .intLit a ∧ r = .intLit b
  · rcases h₁ with ⟨a, b, rfl, rfl⟩
    simpa using foldOrKeep_intLit_intLit op a b
  by_cases h₂ : ∃ a b, l = .boolLit a ∧ r = .boolLit b
  · rcases h₂ with ⟨a, b, rfl, rfl⟩
    simpa using foldOrKeep_boolLit_boolLit op a b
  · simp [foldOrKeep_eq_bin op l r (by simpa using h₁) (by simpa using h₂), eval]

private theorem eval_add_left_zero (r : Expr) (hint : isIntBin .add (.intLit 0) r = true) :
    eval r = eval (.bin .add (.intLit 0) r) := by
  simp [eval, evalBinInt]
  rcases hr : eval r with ⟨⟩ | v
  · rfl
  · rcases v with ⟨_⟩ | ⟨b⟩
    · simp [hr, Option.bind]
    · exfalso
      exact eval_not_bool_of_int r (int_types_of_isIntBin _ _ _ hint).2 (⟨b, hr⟩)

private theorem eval_add_right_zero (l : Expr) (hint : isIntBin .add l (.intLit 0) = true) :
    eval l = eval (.bin .add l (.intLit 0)) := by
  simp [eval, evalBinInt]
  rcases hl : eval l with ⟨⟩ | v
  · rfl
  · rcases v with ⟨_⟩ | ⟨b⟩
    · simp [hl, Option.bind]
    · exfalso
      exact eval_not_bool_of_int l (int_types_of_isIntBin _ _ _ hint).1 (⟨b, hl⟩)

private theorem eval_sub_right_zero (l : Expr) (hint : isIntBin .sub l (.intLit 0) = true) :
    eval l = eval (.bin .sub l (.intLit 0)) := by
  simp [eval, evalBinInt]
  rcases hl : eval l with ⟨⟩ | v
  · rfl
  · rcases v with ⟨_⟩ | ⟨b⟩
    · simp [hl, Option.bind]
    · exfalso
      exact eval_not_bool_of_int l (int_types_of_isIntBin _ _ _ hint).1 (⟨b, hl⟩)

private theorem eval_mul_one_left (r : Expr) (hint : isIntBin .mul (.intLit 1) r = true) :
    eval r = eval (.bin .mul (.intLit 1) r) := by
  simp [eval, evalBinInt]
  rcases hr : eval r with ⟨⟩ | v
  · rfl
  · rcases v with ⟨_⟩ | ⟨b⟩
    · simp [hr, Option.bind]
    · exfalso
      exact eval_not_bool_of_int r (int_types_of_isIntBin _ _ _ hint).2 (⟨b, hr⟩)

private theorem eval_mul_one_right (l : Expr) (hint : isIntBin .mul l (.intLit 1) = true) :
    eval l = eval (.bin .mul l (.intLit 1)) := by
  simp [eval, evalBinInt]
  rcases hl : eval l with ⟨⟩ | v
  · rfl
  · rcases v with ⟨_⟩ | ⟨b⟩
    · simp [hl, Option.bind]
    · exfalso
      exact eval_not_bool_of_int l (int_types_of_isIntBin _ _ _ hint).1 (⟨b, hl⟩)

theorem simplifyBinary_preserves (op : BinOp) (l r : Expr) :
    eval (simplifyBinary op l r) = eval (.bin op l r) := by
  unfold simplifyBinary
  by_cases hadd : op = .add
  · subst hadd
    by_cases hint : isIntBin .add l r
    · by_cases hl0 : l = .intLit 0
      · subst hl0
        simp [hint]
        exact eval_add_left_zero r (by simp [hint])
      · by_cases hr0 : r = .intLit 0
        · subst hr0
          by_cases hl0' : l = .intLit 0
          · subst hl0'
            simp [eval, evalBinInt]
          · simp [hint, hl0, hl0']
            exact eval_add_right_zero l (by simp [hint])
        · simp [hint, hl0, hr0]
          exact foldOrKeep_preserves .add l r
    · simp [hint]
      exact foldOrKeep_preserves .add l r
  by_cases hsub : op = .sub
  · subst hsub
    by_cases hint : isIntBin .sub l r
    · by_cases hr0 : r = .intLit 0
      · subst hr0
        simp [hint]
        exact eval_sub_right_zero l (by simp [hint])
      · simp [hint, hr0]
        exact foldOrKeep_preserves .sub l r
    · simp [hint]
      exact foldOrKeep_preserves .sub l r
  by_cases hmul : op = .mul
  · subst hmul
    by_cases hint : isIntBin .mul l r
    · by_cases hl1 : l = .intLit 1
      · subst hl1
        simp [hint]
        exact eval_mul_one_left r (by simp [hint])
      · by_cases hr1 : r = .intLit 1
        · subst hr1
          by_cases hl1' : l = .intLit 1
          · subst hl1'
            simp [eval, evalBinInt]
          · simp [hint, hl1, hl1']
            exact eval_mul_one_right l (by simp [hint])
        · simp [hint, hl1, hr1]
          exact foldOrKeep_preserves .mul l r
    · simp [hint]
      exact foldOrKeep_preserves .mul l r
  simp [hadd, hsub, hmul]
  exact foldOrKeep_preserves op l r

theorem optimize_preserves (e : Expr) : eval (optimize e) = eval e := by
  induction e with
  | intLit n => simp [optimize, eval]
  | boolLit b => simp [optimize, eval]
  | bin op l r ih_l ih_r =>
    simp only [optimize]
    rw [simplifyBinary_preserves]
    simp only [eval, ih_l, ih_r]

theorem optimize_intLit (n : Int) : optimize (.intLit n) = .intLit n := rfl

theorem optimize_boolLit (b : Bool) : optimize (.boolLit b) = .boolLit b := rfl

theorem fold_add_example :
    optimize (.bin .add (.intLit 2) (.intLit 3)) = .intLit 5 := rfl

theorem fold_mul_example :
    optimize (.bin .mul (.intLit 10) (.intLit 4)) = .intLit 40 := rfl

theorem simplify_add_zero_right (e : Expr) (h : typeOf e = some .int) :
    eval (optimize (.bin .add e (.intLit 0))) = eval (optimize e) := by
  rw [optimize_preserves, optimize_preserves]
  symm
  exact eval_add_right_zero e (by unfold isIntBin; simp [typeOf, h, beq_iff_eq])

theorem simplify_mul_one_right (e : Expr) (h : typeOf e = some .int) :
    eval (optimize (.bin .mul e (.intLit 1))) = eval (optimize e) := by
  rw [optimize_preserves, optimize_preserves]
  symm
  exact eval_mul_one_right e (by unfold isIntBin; simp [typeOf, h, beq_iff_eq])

theorem optimize_preserves_defined (e : Expr) (v : Value) (h : eval e = some v) :
    ∃ v', eval (optimize e) = some v' :=
  ⟨v, by rw [optimize_preserves e, h]⟩

end Forge
