/-!
# Forge process mailbox

Simplified model of `fr_send` / `fr_recv` message passing between light processes.
-/

namespace Forge

/-- A message in a process mailbox: tag + optional owned string payload. -/
structure Message where
  tag : Nat
  payload : Option String
  deriving Repr, DecidableEq

abbrev Mailbox := List Message

/-- Non-blocking receive: returns the first message with matching tag. -/
def recv (mb : Mailbox) (tag : Nat) : Option (Message × Mailbox) :=
  match mb with
  | [] => none
  | m :: rest =>
    if m.tag = tag then
      some (m, rest)
    else do
      let (msg, rest') ← recv rest tag
      pure (msg, m :: rest')

/-- Send appends to the mailbox tail (FIFO). -/
def send (mb : Mailbox) (msg : Message) : Mailbox :=
  mb ++ [msg]

private theorem recv_cons_ne (m : Message) (rest : Mailbox) (tag : Nat)
    (h : recv (m :: rest) tag = none) : m.tag ≠ tag := by
  intro heq
  simp [recv, heq] at h

private theorem recv_cons_tail (m : Message) (rest : Mailbox) (tag : Nat)
    (hne : m.tag ≠ tag) (h : recv (m :: rest) tag = none) :
    recv rest tag = none := by
  rcases hrest : recv rest tag with (_ | ⟨msg, tail⟩)
  · rfl
  · exfalso
    have this : recv (m :: rest) tag = some (msg, m :: tail) := by
      simp [recv, hne, hrest]
    rw [this] at h
    simp at h

theorem send_then_recv (mb : Mailbox) (msg : Message)
    (h : recv mb msg.tag = none) :
    ∃ mb', recv (send mb msg) msg.tag = some (msg, mb') := by
  refine ⟨mb, ?_⟩
  simp only [send]
  induction mb with
  | nil =>
    simp [recv]
  | cons m rest ih =>
    have hne := recv_cons_ne m rest msg.tag h
    have hrest := recv_cons_tail m rest msg.tag hne h
    have ihr : recv (rest ++ [msg]) msg.tag = some (msg, rest) := ih hrest
    show recv (m :: (rest ++ [msg])) msg.tag = some (msg, m :: rest)
    simp [recv, hne, ihr, Option.bind, bind]

end Forge
