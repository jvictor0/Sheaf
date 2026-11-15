import Lake
open Lake DSL
package sheafParameters where
  srcDir := "formal"
  buildDir := "build/formal"
require mathlib from git "https://github.com/leanprover-community/mathlib4.git" @ "v4.32.0"

@[default_target]
lean_lib Sheaf where
  globs := #[.submodules `Sheaf]
  leanOptions := #[⟨`autoImplicit, false⟩, ⟨`warningAsError, true⟩]
