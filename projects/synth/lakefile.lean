import Lake
open Lake DSL
package sheafParameters where
  srcDir := "formal"
  buildDir := "build/formal"
@[default_target]
lean_lib Sheaf where
  globs := #[.submodules `Sheaf]
  leanOptions := #[⟨`autoImplicit, false⟩, ⟨`warningAsError, true⟩]
