import Sheaf.FilterState
import Sheaf.ResponseCore
import Mathlib.Algebra.Order.Round
import Mathlib.Analysis.SpecialFunctions.Log.Base
import Mathlib.Data.Fin.VecNotation

/-! Mathematical round-to-nearest, ties-to-even, with binary32 normal/subnormal
spacing. No NaNs, infinities, overflow or compiler refinement is hidden here:
the regional bound is stated for finite exact operands of magnitude at most 8.
Native tests check the actual implementation's residuals and rounding mode. -/
namespace Sheaf.Rounding
open Sheaf.SourceFilterCoefficients

noncomputable def nearestEven (x : ℝ) : ℤ :=
  let k := Int.floor x
  if x-k < 1/2 then k else
  if 1/2 < x-k then k+1 else
  if k % 2 = 0 then k else k+1

theorem nearest_even_half_unit (x : ℝ) : |(nearestEven x : ℝ)-x| ≤ 1/2 := by
  have hl := Int.floor_le x
  have hu := Int.lt_floor_add_one x
  dsimp only [nearestEven]
  split_ifs <;> (try simp only [Int.cast_add, Int.cast_one]) <;>
    apply abs_le.mpr <;> constructor <;> linarith

noncomputable def gridRound (q x : ℝ) := q * (nearestEven (x/q) : ℝ)

theorem grid_round_half_ulp (q x : ℝ) (hq : 0 < q) : |gridRound q x-x| ≤ q/2 := by
  have he : gridRound q x-x = q*((nearestEven (x/q):ℝ)-x/q) := by
    unfold gridRound
    field_simp
  rw [he, abs_mul, abs_of_pos hq]
  have h := mul_le_mul_of_nonneg_left (nearest_even_half_unit (x/q)) (le_of_lt hq)
  nlinarith

-- A fixed -126 exponent supplies subnormal spacing 2^-149.
noncomputable def binade (x : ℝ) : ℤ :=
  if x = 0 then -126 else max (-126) (Int.floor (Real.logb 2 |x|))
noncomputable def binary32Round (x : ℝ) := gridRound ((2:ℝ)^(binade x-23)) x

theorem finite_region_binade_bound (x : ℝ) (hx : |x| ≤ 8) : binade x ≤ 3 := by
  unfold binade
  split_ifs with hz
  · norm_num
  · have hp : 0 < |x| := abs_pos.mpr hz
    have hl : Real.logb 2 |x| ≤ 3 := by
      apply (Real.logb_le_iff_le_rpow (by norm_num : (1:ℝ) < 2) hp).2
      norm_num
      exact hx
    have hf : (Int.floor (Real.logb 2 |x|) : ℤ) ≤ 3 := by
      exact_mod_cast (Int.floor_le (Real.logb 2 |x|)).trans hl
    exact max_le (by norm_num) hf

theorem finite_binary32_rounding_error (x : ℝ) (hx : |x| ≤ 8) :
    |binary32Round x-x| ≤ 1/2097152 := by
  have he := finite_region_binade_bound x hx
  have hq : 0 < (2:ℝ)^(binade x-23) := by positivity
  have hm : (2:ℝ)^(binade x-23) ≤ (2:ℝ)^(-20:ℤ) :=
    zpow_le_zpow_right₀ (by norm_num) (by omega)
  have hb := grid_round_half_ulp ((2:ℝ)^(binade x-23)) x hq
  unfold binary32Round
  norm_num at hm
  linarith

noncomputable def smallestNormal : ℝ := (2:ℝ)^(-126:ℤ)
noncomputable def coefficientList (s : Biquad) : List ℝ := [s.b0,s.b1,s.b2,s.a1,s.a2]

theorem actual_source_coefficients_zero_or_normal_and_bounded :
    ∀ s ∈ Sheaf.Aliasing.actualSections, ∀ c ∈ coefficientList s,
      (c = 0 ∨ smallestNormal ≤ |c|) ∧ |c| ≤ 2 := by
  norm_num [Sheaf.Aliasing.actualSections, coefficientList, section0, section1,
    section2, section3, section4, section5, smallestNormal]

noncomputable def flushSubnormal (x : ℝ) := if |x| < smallestNormal then 0 else x
noncomputable def binary32RoundFtz (x : ℝ) := flushSubnormal (binary32Round x)

theorem flush_error_bound (x : ℝ) : |flushSubnormal x-x| ≤ smallestNormal := by
  unfold flushSubnormal
  split_ifs with h
  · simpa using le_of_lt h
  · simp only [sub_self, abs_zero]
    unfold smallestNormal
    positivity

theorem finite_binary32_ftz_error (x : ℝ) (hx : |x| ≤ 8) :
    |binary32RoundFtz x-x| ≤ 1/2097152+smallestNormal := by
  have hf := flush_error_bound (binary32Round x)
  have hr := finite_binary32_rounding_error x hx
  have ht := abs_add_le (flushSubnormal (binary32Round x)-binary32Round x) (binary32Round x-x)
  unfold binary32RoundFtz
  simp only [sub_add_sub_cancel] at ht
  linarith

theorem flush_idempotent (x : ℝ) : flushSubnormal (flushSubnormal x) = flushSubnormal x := by
  unfold flushSubnormal
  split_ifs <;> simp_all

theorem daz_product_error (coefficient value : ℝ) (hc : |coefficient| ≤ 2) :
    |coefficient*flushSubnormal value-coefficient*value| ≤ 2*smallestNormal := by
  rw [← mul_sub, abs_mul]
  exact (mul_le_mul_of_nonneg_left (flush_error_bound value) (abs_nonneg coefficient)).trans
    (mul_le_mul_of_nonneg_right hc (by unfold smallestNormal; positivity))

theorem finite_rounding_result_bound (x : ℝ) (hx : |x| ≤ 8) : |binary32Round x| ≤ 9 := by
  have hr := finite_binary32_rounding_error x hx
  have ht : |binary32Round x| ≤ |binary32Round x-x|+|x| := by
    simpa using abs_add_le (binary32Round x-x) x
  linarith

theorem ftz_regional_rounding_bound (x : ℝ) (hx : |x| ≤ 8) :
    |binary32RoundFtz x-x| ≤ 1/1048576 := by
  have hr := finite_binary32_ftz_error x hx
  have hs : smallestNormal ≤ 1/2097152 := by norm_num [smallestNormal]
  linarith

noncomputable def binade64 (x : ℝ) : ℤ :=
  if x = 0 then -1022 else max (-1022) (Int.floor (Real.logb 2 |x|))
noncomputable def binary64Round (x : ℝ) := gridRound ((2:ℝ)^(binade64 x-52)) x

theorem finite_binary64_rounding_error (x : ℝ) (hx : |x| ≤ 16) :
    |binary64Round x-x| ≤ 1/562949953421312 := by
  have he : binade64 x ≤ 4 := by
    unfold binade64
    split_ifs with hz
    · norm_num
    · have hp : 0 < |x| := abs_pos.mpr hz
      have hl : Real.logb 2 |x| ≤ 4 := by
        apply (Real.logb_le_iff_le_rpow (by norm_num : (1:ℝ) < 2) hp).2
        norm_num
        exact hx
      have hf : (Int.floor (Real.logb 2 |x|):ℤ) ≤ 4 := by
        exact_mod_cast (Int.floor_le (Real.logb 2 |x|)).trans hl
      exact max_le (by norm_num) hf
  have hq : 0 < (2:ℝ)^(binade64 x-52) := by positivity
  have hm : (2:ℝ)^(binade64 x-52) ≤ (2:ℝ)^(-48:ℤ) :=
    zpow_le_zpow_right₀ (by norm_num) (by omega)
  have hb := grid_round_half_ulp ((2:ℝ)^(binade64 x-52)) x hq
  unfold binary64Round
  norm_num at hm
  linarith

noncomputable def binary64RoundFtz (x : ℝ) :=
  let rounded := binary64Round x
  if |rounded| < (2:ℝ)^(-1022:ℤ) then 0 else rounded

theorem ftz_binary64_regional_bound (x : ℝ) (hx : |x| ≤ 16) :
    |binary64RoundFtz x-x| ≤ 1/281474976710656 := by
  have hr := finite_binary64_rounding_error x hx
  have hs : (2:ℝ)^(-1022:ℤ) ≤ 1/562949953421312 := by
    calc
      _ ≤ (2:ℝ)^(-49:ℤ) := zpow_le_zpow_right₀ (by norm_num) (by norm_num)
      _ = _ := by norm_num
  unfold binary64RoundFtz
  dsimp only
  split_ifs with h
  · have ht := abs_add_le (x-binary64Round x) (binary64Round x)
    rw [abs_sub_comm] at hr
    simp only [sub_add_cancel] at ht
    simp only [zero_sub, abs_neg]
    linarith
  · linarith

-- Products 0..4 and left-associated add/subtract roundings 5..8 correspond
-- to BiquadSection::Process. A fused multiply-add needs fewer roundings;
-- the residual bound below also applies whenever its local residual fits it.
noncomputable def tracedBiquad (s : Biquad) (input x1 x2 y1 y2 : ℝ) (e : Fin 9 → ℝ) :=
  ((((s.b0*input+e 0)+(s.b1*x1+e 1)+e 5)+(s.b2*x2+e 2)+e 6)-
    (s.a1*y1+e 3)+e 7)-(s.a2*y2+e 4)+e 8
noncomputable def exactBiquad (s : Biquad) (input x1 x2 y1 y2 : ℝ) :=
  s.b0*input+s.b1*x1+s.b2*x2-s.a1*y1-s.a2*y2

theorem nine_operation_residual (s : Biquad) (input x1 x2 y1 y2 ε : ℝ)
    (e : Fin 9 → ℝ) (he : ∀ i, |e i| ≤ ε) :
    |tracedBiquad s input x1 x2 y1 y2 e-exactBiquad s input x1 x2 y1 y2| ≤ 9*ε := by
  have h0 := abs_le.mp (he 0)
  have h1 := abs_le.mp (he 1)
  have h2 := abs_le.mp (he 2)
  have h3 := abs_le.mp (he 3)
  have h4 := abs_le.mp (he 4)
  have h5 := abs_le.mp (he 5)
  have h6 := abs_le.mp (he 6)
  have h7 := abs_le.mp (he 7)
  have h8 := abs_le.mp (he 8)
  apply abs_le.mpr
  unfold tracedBiquad exactBiquad
  constructor <;> linarith

theorem rounded_operation_residual (s : Biquad) (input x1 x2 y1 y2 : ℝ)
    (operands : Fin 9 → ℝ) (bound : ∀ i, |operands i| ≤ 8) :
    |tracedBiquad s input x1 x2 y1 y2 (fun i => binary32Round (operands i)-operands i)-
      exactBiquad s input x1 x2 y1 y2| ≤ 9/2097152 := by
  have h := nine_operation_residual s input x1 x2 y1 y2 (1/2097152)
    (fun i => binary32Round (operands i)-operands i) (fun i => finite_binary32_rounding_error _ (bound i))
  simpa only [div_eq_mul_inv, one_mul] using h

noncomputable def operationInputs (s : Biquad) (input x1 x2 y1 y2 : ℝ) : Fin 9 → ℝ :=
  let p0 := s.b0*input
  let p1 := s.b1*x1
  let p2 := s.b2*x2
  let p3 := s.a1*y1
  let p4 := s.a2*y2
  let a5 := binary32Round p0 + binary32Round p1
  let a6 := binary32Round a5 + binary32Round p2
  let a7 := binary32Round a6 - binary32Round p3
  let a8 := binary32Round a7 - binary32Round p4
  ![p0, p1, p2, p3, p4, a5, a6, a7, a8]

noncomputable def executeRounded (s : Biquad) (input x1 x2 y1 y2 : ℝ) :=
  binary32Round (operationInputs s input x1 x2 y1 y2 8)

theorem rounded_execution_is_trace (s : Biquad) (input x1 x2 y1 y2 : ℝ) :
    executeRounded s input x1 x2 y1 y2 =
      tracedBiquad s input x1 x2 y1 y2 (fun i =>
        binary32Round (operationInputs s input x1 x2 y1 y2 i)-operationInputs s input x1 x2 y1 y2 i) := by
  dsimp [executeRounded, operationInputs, tracedBiquad]
  ring

theorem executed_binary32_local_bound (s : Biquad) (input x1 x2 y1 y2 : ℝ)
    (bound : ∀ i, |operationInputs s input x1 x2 y1 y2 i| ≤ 8) :
    |executeRounded s input x1 x2 y1 y2 - exactBiquad s input x1 x2 y1 y2| ≤ 9/2097152 := by
  rw [rounded_execution_is_trace]
  exact rounded_operation_residual s input x1 x2 y1 y2 _ bound

-- Five products can each lose at most 2*smallestNormal to DAZ with
-- zero or normal coefficients of magnitude at most two, plus one FTZ result.
-- Four add/subtract results each add one FTZ result allowance.
theorem nine_operation_ftz_daz_residual (s : Biquad) (input x1 x2 y1 y2 ε : ℝ)
    (e : Fin 9 → ℝ)
    (he : ∀ i, |e i| ≤ ε+(if i.val < 5 then 3 else 1)*smallestNormal) :
    |tracedBiquad s input x1 x2 y1 y2 e-exactBiquad s input x1 x2 y1 y2| ≤
      9*ε+19*smallestNormal := by
  have h0 := abs_le.mp (he 0)
  have h1 := abs_le.mp (he 1)
  have h2 := abs_le.mp (he 2)
  have h3 := abs_le.mp (he 3)
  have h4 := abs_le.mp (he 4)
  have h5 := abs_le.mp (he 5)
  have h6 := abs_le.mp (he 6)
  have h7 := abs_le.mp (he 7)
  have h8 := abs_le.mp (he 8)
  norm_num at h0 h1 h2 h3 h4 h5 h6 h7 h8
  apply abs_le.mpr
  unfold tracedBiquad exactBiquad
  constructor <;> linarith

-- Link the local rounding residual to the previously proved state budget.
theorem rounding_enters_state_budget {s : Biquad} (hs : Sheaf.FilterState.Certificate s)
    (initial : Sheaf.FilterState.State) (d : ℕ → ℝ)
    (hd : ∀ n, |d n| ≤ 9/2097152) (n : ℕ) :
    (Sheaf.FilterState.run s initial d n).recent^2 ≤
      1000*((9/10:ℝ)^n*Sheaf.FilterState.energy s initial.recent initial.previous +
      90*(9/2097152:ℝ)^2*(1-(9/10:ℝ)^n)) :=
  Sheaf.FilterState.component_error_budget hs initial d (9/2097152) hd n

theorem upstream_and_rounding_disturbance (s : Biquad) (u x1 x2 r Δ η : ℝ)
    (hu : |u| ≤ Δ) (h1 : |x1| ≤ Δ) (h2 : |x2| ≤ Δ) (hr : |r| ≤ η) :
    |s.b0*u+s.b1*x1+s.b2*x2+r| ≤ (|s.b0|+|s.b1|+|s.b2|)*Δ+η := by
  have h0 := mul_le_mul_of_nonneg_left hu (abs_nonneg s.b0)
  have hb1 := mul_le_mul_of_nonneg_left h1 (abs_nonneg s.b1)
  have hb2 := mul_le_mul_of_nonneg_left h2 (abs_nonneg s.b2)
  have ha := abs_add_le (s.b0*u) (s.b1*x1)
  have hb := abs_add_le (s.b0*u+s.b1*x1) (s.b2*x2)
  have hc := abs_add_le (s.b0*u+s.b1*x1+s.b2*x2) r
  simp only [abs_mul] at ha hb hc
  nlinarith

end Sheaf.Rounding
