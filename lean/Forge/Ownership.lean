/-!
# Forge ownership model

Formal model of `own let`, `move(x)`, and `fr_own_take` (`runtime/ownership.c`,
`include/forge/ownership.h`).  A variable is either **live** (usable) or **moved**
(consumed).
-/

namespace Forge

/-- Ownership state of a heap string binding. -/
inductive OwnState
  | live
  | moved
  deriving Repr, DecidableEq

/-- Environment mapping variable names to ownership state. -/
abbrev OwnEnv := List (String × OwnState)

def OwnEnv.lookup (env : OwnEnv) (name : String) : Option OwnState :=
  (env.find? fun p => p.1 = name).map Prod.snd

def OwnEnv.set (env : OwnEnv) (name : String) (st : OwnState) : OwnEnv :=
  let rest := env.filter fun p => p.1 ≠ name
  (name, st) :: rest

/-- Result of an ownership check or transfer. -/
inductive OwnResult
  | ok (env : OwnEnv)
  | err (reason : String)
  deriving Repr

def OwnResult.isErr : OwnResult → Bool
  | .ok _ => false
  | .err _ => true

/-- Read a variable: only allowed when still live. -/
def ownRead (env : OwnEnv) (name : String) : OwnResult :=
  match env.lookup name with
  | some .live  => .ok env
  | some .moved => .err s!"use after move: {name}"
  | none        => .err s!"unknown variable: {name}"

/--
Transfer ownership (`fr_own_take`): the source slot becomes `moved`,
and the caller receives the payload.
-/
def ownTake (env : OwnEnv) (name : String) : OwnResult × Option String :=
  match env.lookup name with
  | some .live  => (.ok (env.set name .moved), some name)
  | some .moved => (.err s!"use after move: {name}", none)
  | none        => (.err s!"unknown variable: {name}", none)

/--
`send proc, tag, move(msg)` — payload must be live before the send;
afterwards the local binding is moved.
-/
def ownSendMove (env : OwnEnv) (name : String) : OwnResult :=
  match ownTake env name with
  | (.ok env', some _) => .ok env'
  | (.err r, _)        => .err r
  | _                  => .err "internal ownership error"

private theorem lookup_set_self (env : OwnEnv) (name : String) (st : OwnState) :
    (env.set name st).lookup name = some st := by
  simp [OwnEnv.lookup, OwnEnv.set]

theorem ownTake_marks_moved (env : OwnEnv) (name : String)
    (h : env.lookup name = some .live) :
    ∃ env', (ownTake env name).1 = .ok env' ∧
      env'.lookup name = some .moved := by
  simp only [ownTake, h]
  refine ⟨_, rfl, ?_⟩
  exact lookup_set_self env name .moved

theorem ownRead_after_take_fails (env : OwnEnv) (name : String)
    (h : env.lookup name = some .live) :
    ∃ env', (ownTake env name).1 = .ok env' ∧
      (ownRead env' name).isErr = true := by
  obtain ⟨env', htake, hmoved⟩ := ownTake_marks_moved env name h
  refine ⟨env', htake, ?_⟩
  simp [ownRead, hmoved, OwnResult.isErr]

theorem ownTake_idempotent_on_moved (env : OwnEnv) (name : String)
    (h : env.lookup name = some .moved) :
    (ownTake env name).1.isErr = true := by
  simp [ownTake, h, OwnResult.isErr]

/-- Double-move is rejected (compiler + runtime invariant). -/
theorem no_double_move (env : OwnEnv) (name : String)
    (hlive : env.lookup name = some .live) :
    match (ownTake env name).1 with
    | .ok env' => (ownTake env' name).1.isErr = true
    | .err _   => true := by
  obtain ⟨env', htake, hmoved⟩ := ownTake_marks_moved env name hlive
  simp only [htake]
  exact ownTake_idempotent_on_moved env' name hmoved

end Forge
