import Sheaf.SourceFilterCoefficients
import Mathlib.Analysis.SpecialFunctions.Trigonometric.Bounds
import Mathlib.Tactic.NormNum
import Mathlib.Tactic.Linarith
import Mathlib.Tactic.Ring
import Mathlib.Tactic.Positivity

/-! Exact-real steady-state response only. Scalar and section-major native kernels
share these coefficients; no compiler or floating-point refinement is asserted. -/
namespace Sheaf.Aliasing
open Sheaf.SourceFilterCoefficients

noncomputable def actualSections : List Biquad :=
  [section0, section1, section2, section3, section4, section5]

noncomputable def numerator (s : Biquad) (c : ℝ) : ℝ :=
  s.b0^2 + s.b1^2 + s.b2^2 + 2*s.b1*(s.b0+s.b2)*c + 2*s.b0*s.b2*(2*c^2-1)
noncomputable def denominator (s : Biquad) (c : ℝ) : ℝ :=
  1 + s.a1^2 + s.a2^2 + 2*s.a1*(1+s.a2)*c + 2*s.a2*(2*c^2-1)
noncomputable def power (s : Biquad) (c : ℝ) : ℝ := numerator s c / denominator s c
noncomputable def directPower (s : Biquad) (ω : ℝ) : ℝ :=
  ((s.b0 + s.b1*Real.cos ω + s.b2*Real.cos (2*ω))^2 +
    (s.b1*Real.sin ω + s.b2*Real.sin (2*ω))^2) /
  ((1 + s.a1*Real.cos ω + s.a2*Real.cos (2*ω))^2 +
    (s.a1*Real.sin ω + s.a2*Real.sin (2*ω))^2)

theorem direct_response_matches_polynomial (s : Biquad) (ω : ℝ) :
    directPower s ω = power s (Real.cos ω) := by
  have h := Real.sin_sq_add_cos_sq ω
  have hsin : Real.sin ω ^ 2 = 1 - Real.cos ω ^ 2 := by nlinarith
  unfold directPower power numerator denominator
  rw [Real.cos_two_mul, Real.sin_two_mul]
  congr 1 <;> ring_nf <;> rw [hsin] <;> ring

theorem numerator_nonnegative (s : Biquad) (c : ℝ) (lo : -1 ≤ c) (hi : c ≤ 1) :
    0 ≤ numerator s c := by
  have hc : 0 ≤ 1-c^2 := by nlinarith
  have hid : numerator s c = (s.b1+(s.b0+s.b2)*c)^2 + (s.b0-s.b2)^2*(1-c^2) := by
    unfold numerator; ring
  rw [hid]
  positivity

theorem quadratic_denominator_positive (s : Biquad) (c : ℝ)
    (ha : 0 < s.a2) (hd : 0 < 4*s.a2-s.a1^2) (hne : s.a2 ≠ 1) :
    0 < denominator s c := by
  have hid : 4*s.a2*denominator s c =
      (4*s.a2*c+s.a1*(1+s.a2))^2 + (1-s.a2)^2*(4*s.a2-s.a1^2) := by
    unfold denominator; ring
  have hn : 0 < (1-s.a2)^2 := sq_pos_of_ne_zero (by intro h; apply hne; linarith)
  have hp := mul_pos hn hd
  nlinarith [sq_nonneg (4*s.a2*c+s.a1*(1+s.a2))]

theorem actual_denominator_positive (s : Biquad) (hs : s ∈ actualSections)
    (c : ℝ) (_lo : -1 ≤ c) (hi : c ≤ 1) : 0 < denominator s c := by
  simp only [actualSections, List.mem_cons, List.not_mem_nil, or_false] at hs
  rcases hs with h | h | h | h | h | h <;> subst s
  · have hb := mul_le_mul_of_nonpos_left hi (by norm_num : (-13320623/16777216:ℝ) ≤ 0)
    norm_num [denominator, section0] at *
    linarith
  all_goals apply quadratic_denominator_positive <;>
    norm_num [section1, section2, section3, section4, section5]

noncomputable def actualNumerator (c : ℝ) := (actualSections.map (fun s => numerator s c)).prod
noncomputable def actualDenominator (c : ℝ) := (actualSections.map (fun s => denominator s c)).prod
noncomputable def cascadePower (c : ℝ) := (actualSections.map (fun s => power s c)).prod
noncomputable def cascadeResponsePower (ω : ℝ) := (actualSections.map (fun s => directPower s ω)).prod

theorem cascade_response_matches_polynomial (ω : ℝ) :
    cascadeResponsePower ω = cascadePower (Real.cos ω) := by
  simp [cascadeResponsePower, cascadePower, direct_response_matches_polynomial]

theorem cascade_is_quotient (c : ℝ) : cascadePower c = actualNumerator c / actualDenominator c := by
  simp [cascadePower, actualNumerator, actualDenominator, actualSections, power, div_mul_div_comm]

theorem actual_denominator_product_positive (c : ℝ) (lo : -1 ≤ c) (hi : c ≤ 1) :
    0 < actualDenominator c := by
  apply List.prod_pos
  intro d hd
  obtain ⟨s, hs, rfl⟩ := List.mem_map.mp hd
  exact actual_denominator_positive s hs c lo hi

theorem actual_numerator_product_nonnegative (c : ℝ) (lo : -1 ≤ c) (hi : c ≤ 1) :
    0 ≤ actualNumerator c := by
  apply List.prod_nonneg
  intro d hd
  obtain ⟨s, _, rfl⟩ := List.mem_map.mp hd
  exact numerator_nonnegative s c lo hi

-- One affine transport serves every generated, closed Bernstein interval.
theorem interval_transfer (p : ℝ → ℝ) (l r c : ℝ) (hlr : l < r)
    (hc : l ≤ c) (hr : c ≤ r)
    (leaf : ∀ t : ℝ, 0 ≤ t → t ≤ 1 → 0 ≤ p (l+(r-l)*t)) : 0 ≤ p c := by
  let t := (c-l)/(r-l)
  have hpos : 0 < r-l := by linarith
  have ht : 0 ≤ t := div_nonneg (by linarith) (le_of_lt hpos)
  have hu : t ≤ 1 := (div_le_iff₀ hpos).2 (by linarith)
  have he : l+(r-l)*t = c := by dsimp [t]; field_simp; ring
  simpa only [he] using leaf t ht hu
end Sheaf.Aliasing
