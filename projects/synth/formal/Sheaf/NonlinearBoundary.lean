import Mathlib.Algebra.Order.Archimedean.Real.Basic
import Mathlib.Tactic.FieldSimp
import Mathlib.Tactic.Positivity
import Mathlib.Algebra.Order.Floor.Semiring
import Mathlib.Tactic.NormNum
import Mathlib.Tactic.Linarith
import Mathlib.Tactic.Ring

/-! Exact integer core of Fuegoize at knob=1, row=0. The real extension below
models ideal input scaling/floor/remainder. Native adjacent-float tests separately
check the concrete C++ discontinuity. This is a boundary result, not a claim
that the entire finite binary32 input set has no finite Lipschitz constant. -/
namespace Sheaf.NonlinearBoundary

def scramble (n : ℕ) : ℕ :=
  let b := n % 256
  let b := Nat.xor b ((b*8)%256)
  let b := Nat.xor b ((b/32)%256)
  let b := Nat.xor b ((b*2)%256)
  Nat.xor b ((b/2)%256)

noncomputable def fuego (x : ℝ) : ℝ :=
  let n := Nat.floor (255*x)
  ((scramble n : ℝ)+(255*x-(n:ℝ)))/255

theorem first_bit_cells : scramble 0 = 0 ∧ scramble 1 = 22 := by decide

theorem below_first_threshold (ε : ℝ) (positive : 0 < ε) (small : ε < 1/255) :
    fuego (1/255-ε) = 1/255-ε := by
  have hdomain : 0 ≤ 255*(1/255-ε) := by linarith
  have hf : Nat.floor (255*(1/255-ε)) = 0 :=
    (Nat.floor_eq_iff hdomain).mpr ⟨by norm_num; linarith, by norm_num; linarith⟩
  simp only [fuego, hf, first_bit_cells.1, Nat.cast_zero]
  ring

theorem at_first_threshold : fuego (1/255) = 22/255 := by
  norm_num [fuego, first_bit_cells.2]

theorem threshold_jump (ε : ℝ) (positive : 0 < ε) (small : ε < 1/255) :
    fuego (1/255)-fuego (1/255-ε) = 21/255+ε := by
  rw [at_first_threshold, below_first_threshold ε positive small]
  ring

-- Any proposed finite gain L is exceeded by a sufficiently small perturbation
-- at the actual first bit-cell boundary of the real extension.
theorem no_uniform_small_error_gain (L : ℝ) (hL : 0 ≤ L) :
    ∃ ε : ℝ, 0 < ε ∧ ε < 1/255 ∧
      L*ε < |fuego (1/255)-fuego (1/255-ε)| := by
  let ε := 1/(510*(L+1))
  have hpos : 0 < ε := by dsimp [ε]; positivity
  have hid : ε*(510*(L+1)) = 1 := by dsimp [ε]; field_simp
  have hsmall : ε < 1/255 := by nlinarith
  refine ⟨ε, hpos, hsmall, ?_⟩
  rw [threshold_jump ε hpos hsmall, abs_of_pos (by linarith : 0 < 21/255+ε)]
  nlinarith

-- The code function freezes the integer transformation selected by a knob
-- mask and row. This contract applies to all such code functions, not only
-- the concrete row-zero witness above.
noncomputable def cellWarp (code : ℕ → ℕ) (x : ℝ) :=
  let n := Nat.floor (255*x)
  ((code n:ℝ)+(255*x-(n:ℝ)))/255

noncomputable def cellOffset (code : ℕ → ℕ) (x : ℝ) :=
  ((code (Nat.floor (255*x)):ℝ)-(Nat.floor (255*x):ℝ))/255

theorem cell_warp_offset_form (code : ℕ → ℕ) (x : ℝ) :
    cellWarp code x = x+cellOffset code x := by
  unfold cellWarp cellOffset
  ring

theorem same_code_and_cell_preserve_error (code : ℕ → ℕ) (x y : ℝ)
    (same : Nat.floor (255*x) = Nat.floor (255*y)) :
    |cellWarp code x-cellWarp code y| = |x-y| := by
  rw [cell_warp_offset_form, cell_warp_offset_form]
  unfold cellOffset
  rw [same]
  congr 1
  ring

theorem explicit_cell_crossing_budget (codeA codeB : ℕ → ℕ) (x y : ℝ) :
    |cellWarp codeA x-cellWarp codeB y| ≤
      |x-y|+|cellOffset codeA x-cellOffset codeB y| := by
  have he : cellWarp codeA x-cellWarp codeB y =
      (x-y)+(cellOffset codeA x-cellOffset codeB y) := by
    rw [cell_warp_offset_form, cell_warp_offset_form]
    ring
  rw [he]
  exact abs_add_le _ _

theorem two_stage_cell_error_budget (globalA globalB localA localB : ℕ → ℕ) (x y : ℝ) :
    |cellWarp localA (cellWarp globalA x)-cellWarp localB (cellWarp globalB y)| ≤
      |x-y|+|cellOffset globalA x-cellOffset globalB y|+
      |cellOffset localA (cellWarp globalA x)-cellOffset localB (cellWarp globalB y)| := by
  have hg := explicit_cell_crossing_budget globalA globalB x y
  have hl := explicit_cell_crossing_budget localA localB (cellWarp globalA x) (cellWarp globalB y)
  linarith

-- A linear attenuation guarantee cannot be transported through an arbitrary
-- nonlinear function without a separate stage contract. The counterexample
-- above is tied to Fuego's implemented integer transformation.
noncomputable def hardBound (x : ℝ) := min 1 (max (-1) x)

theorem final_bound_range (x : ℝ) : -1 ≤ hardBound x ∧ hardBound x ≤ 1 := by
  unfold hardBound
  constructor
  · exact le_min (by norm_num) (le_max_left _ _)
  · exact min_le_left _ _

theorem final_bound_does_not_amplify_error (x y : ℝ) :
    |hardBound x-hardBound y| ≤ |x-y| := by
  unfold hardBound
  simp only [min_def, max_def]
  split_ifs <;> apply abs_le.mpr <;> constructor <;>
    linarith [abs_nonneg (x-y), le_abs_self (x-y), neg_le_abs (x-y)]

end Sheaf.NonlinearBoundary
