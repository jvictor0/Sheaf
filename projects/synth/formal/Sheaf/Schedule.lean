import Std

namespace Sheaf.Schedule

/-! Adapted from jvictor0/theallelectricsmartgrid, revision
 e7afa9761bb2238b5affc3c61d92be27aec88476, formal/Encoder/Schedule.lean.
 GPL-3.0; see COPYING.smart-grid. This is a reusable evaluation-order theorem,
 not a claim that Sheaf currently builds a prepared schedule. The production
 integration deliberately retains Sheaf's recursive evaluator.
-/
inductive Expr (α : Type) (before : Nat) where
  | literal : α → Expr α before
  | read : Fin before → Expr α before
  | unary : (α → α) → Expr α before → Expr α before
  | binary : (α → α → α) → Expr α before → Expr α before → Expr α before

def Expr.eval {α : Type} {n : Nat} (values : Nat → α) : Expr α n → α
  | .literal a => a
  | .read i => values i.val
  | .unary f e => f (e.eval values)
  | .binary f a b => f (a.eval values) (b.eval values)

theorem Expr.depends_only_on_earlier {α : Type} {n : Nat}
    (e : Expr α n) (a b : Nat → α) (h : ∀ j, j < n → a j = b j) :
    e.eval a = e.eval b := by
  induction e with
  | literal => rfl
  | read i => exact h i.val i.isLt
  | unary f e ih => simp [Expr.eval, ih]
  | binary f a b ia ib => simp [Expr.eval, ia, ib]

abbrev Program (α : Type) := (i : Nat) → Expr α i

def recursive {α : Type} [Inhabited α] (p : Program α) (i : Nat) : α :=
  (p i).eval (fun j => if _h : j < i then recursive p j else default)
termination_by i

-- Each iteration writes one node into the contiguous output array.
def prepared {α : Type} (p : Program α) (seed : Nat → α) : Nat → Nat → α
  | 0 => seed
  | n+1 => fun j => if j = n then (p n).eval (prepared p seed n) else prepared p seed n j

theorem prepared_equals_recursive {α : Type} [Inhabited α]
    (p : Program α) (seed : Nat → α) (n : Nat) :
    ∀ i, i < n → prepared p seed n i = recursive p i := by
  induction n with
  | zero => intro i h; omega
  | succ n ih =>
    intro i hi
    by_cases hin : i = n
    · subst i
      simp only [prepared, ↓reduceIte]
      rw [recursive]
      apply Expr.depends_only_on_earlier
      intro j hj
      simp only [hj, ↓reduceDIte]
      exact ih j hj
    · simp only [prepared, hin, ↓reduceIte]
      exact ih i (by omega)

-- A dependency-independent node can retain its cached output.
theorem unaffected_node_can_skip {α : Type} {n : Nat} (e : Expr α n)
    (old now : Nat → α) (unchanged : ∀ i, i < n → old i = now i) :
    e.eval old = e.eval now := Expr.depends_only_on_earlier e old now unchanged

end Sheaf.Schedule
