import Forge.AST
import Forge.Eval
import Forge.Optimize
import Forge.OptimizeCorrect
import Forge.Ownership
import Forge.Match
import Forge.Mailbox

/-!
# Forge formal verification

Root module for the Lean 4 proofs accompanying the [Forge](https://github.com/Helloworld0822/forge)
language implementation.

## Verified components

| Module | C counterpart | Property |
|--------|---------------|----------|
| `Forge.OptimizeCorrect` | `compiler/optimize.c` | constant folding / algebraic simplification preserves semantics |
| `Forge.Ownership` | `runtime/ownership.c` | move transfers ownership; use-after-move is rejected |
| `Forge.Match` | `examples/match.fg` | first matching `match` arm is selected |
| `Forge.Mailbox` | `runtime/event_loop.c` (mailbox) | send then recv delivers the message |

## Build

```bash
cd lean
lake update
lake build
```

Requires [Lean 4](https://leanprover.github.io/) ≥ 4.14.0 (`elan` recommended).
-/
