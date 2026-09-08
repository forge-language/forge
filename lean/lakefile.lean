import Lake
open Lake DSL

package «forge-proofs» where
  leanOptions := #[
    ⟨`autoImplicit, false⟩,
    ⟨`relaxedAutoImplicit, false⟩
  ]

@[default_target]
lean_lib «forge-proofs» where
  globs := #[.submodules `Forge]
