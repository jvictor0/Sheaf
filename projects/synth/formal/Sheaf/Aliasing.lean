import Sheaf.SourceFilterCertificate
import Mathlib.Analysis.Real.Pi.Bounds

/-! Complete closed-band certificate for the six delivered sections. Exact-real
steady-state response only; native arithmetic, initialization, FOH, publication
limiting and nonlinear paths remain separate contracts. -/
namespace Sheaf.Aliasing
open Sheaf.SourceFilterCertificate

theorem stopband_power_bound (c : ℝ) (lo : -1 ≤ c) (hi : c ≤ 707107/1000000) :
    0 ≤ cascadePower c ∧ cascadePower c ≤ 981/1000000000000 := by
  have hc : c ≤ 1 := by linarith
  have hd := actual_denominator_product_positive c lo hc
  have hn := actual_numerator_product_nonnegative c lo hc
  have hs := stop_whole c (by simpa using lo) hi
  rw [stopPolynomial, N_matches_actual, D_matches_actual] at hs
  rw [cascade_is_quotient]
  exact ⟨div_nonneg hn hd.le, (div_le_iff₀ hd).2 (by linarith)⟩

theorem passband_power_bounds (c : ℝ) (lo : 793353/1000000 ≤ c) (hi : c ≤ 1) :
    981/1000 ≤ cascadePower c ∧ cascadePower c ≤ 10001/10000 := by
  have hc : -1 ≤ c := by linarith
  have hd := actual_denominator_product_positive c hc hi
  have hl := lower_whole c lo (by simpa using hi)
  have hu := upper_whole c lo (by simpa using hi)
  rw [lowerPolynomial, N_matches_actual, D_matches_actual] at hl
  rw [upperPolynomial, N_matches_actual, D_matches_actual] at hu
  rw [cascade_is_quotient]
  exact ⟨(le_div_iff₀ hd).2 (by linarith), (div_le_iff₀ hd).2 (by linarith)⟩

-- Exact angle addition and double-angle identities reduce the endpoint to
-- rational square bounds on sqrt 2 and sqrt 3; no numerical cosine oracle.
theorem pass_endpoint_cosine : 793353/1000000 ≤ Real.cos (5*Real.pi/24) := by
  have h2 := Real.sq_sqrt (by norm_num : (0:ℝ) ≤ 2)
  have h3 := Real.sq_sqrt (by norm_num : (0:ℝ) ≤ 3)
  have h2n := Real.sqrt_nonneg (2:ℝ)
  have h3n := Real.sqrt_nonneg (3:ℝ)
  have h2l : (1414213/1000000:ℝ) ≤ Real.sqrt 2 := by nlinarith
  have h3l : (1732050/1000000:ℝ) ≤ Real.sqrt 3 := by nlinarith
  have hp := mul_le_mul h2l (sub_le_sub_right h3l 1) (by norm_num : (0:ℝ) ≤ 1732050/1000000-1) h2n
  have ha : 2*(5*Real.pi/24) = Real.pi/4+Real.pi/6 := by ring
  have hc := Real.cos_two_mul (5*Real.pi/24)
  rw [ha, Real.cos_add, Real.cos_pi_div_four, Real.sin_pi_div_four,
    Real.cos_pi_div_six, Real.sin_pi_div_six] at hc
  have hn := Real.cos_nonneg_of_mem_Icc (x := 5*Real.pi/24)
    (show 5*Real.pi/24 ∈ Set.Icc (-(Real.pi/2)) (Real.pi/2) from
      ⟨by linarith [Real.pi_pos], by linarith [Real.pi_pos]⟩)
  nlinarith

theorem stop_endpoint_cosine : Real.cos (Real.pi/4) ≤ 707107/1000000 := by
  rw [Real.cos_pi_div_four]
  have hs := Real.sq_sqrt (by norm_num : (0:ℝ) ≤ 2)
  have hn := Real.sqrt_nonneg (2:ℝ)
  nlinarith

-- r denotes cycles per engine sample. At 192 kHz these closed bands are
-- 0..20 kHz and 24..96 kHz; negotiated rates scale the physical edges.
theorem low_frequency_band_bounds (r : ℝ) (lo : 0 ≤ r) (hi : r ≤ 5/48) :
    981/1000 ≤ cascadeResponsePower (2*Real.pi*r) ∧
    cascadeResponsePower (2*Real.pi*r) ≤ 10001/10000 := by
  rw [cascade_response_matches_polynomial]
  have ha : 0 ≤ 2*Real.pi*r := by positivity
  have hb : 5*Real.pi/24 ≤ Real.pi := by linarith [Real.pi_pos]
  have hab : 2*Real.pi*r ≤ 5*Real.pi/24 := by
    nlinarith [mul_nonneg Real.pi_pos.le (show 0 ≤ 5/48-r by linarith)]
  have hc := Real.cos_le_cos_of_nonneg_of_le_pi ha hb hab
  exact passband_power_bounds _ (pass_endpoint_cosine.trans hc) (Real.cos_le_one _)

theorem high_frequency_band_bound (r : ℝ) (lo : 1/8 ≤ r) (hi : r ≤ 1/2) :
    0 ≤ cascadeResponsePower (2*Real.pi*r) ∧
    cascadeResponsePower (2*Real.pi*r) ≤ 981/1000000000000 := by
  rw [cascade_response_matches_polynomial]
  have ha : 0 ≤ Real.pi/4 := by positivity
  have hb : 2*Real.pi*r ≤ Real.pi := by
    nlinarith [mul_nonneg Real.pi_pos.le (show 0 ≤ 1/2-r by linarith)]
  have hab : Real.pi/4 ≤ 2*Real.pi*r := by
    nlinarith [mul_nonneg Real.pi_pos.le (show 0 ≤ r-1/8 by linarith)]
  have hc := Real.cos_le_cos_of_nonneg_of_le_pi ha hb hab
  exact stopband_power_bound _ (Real.neg_one_le_cos _) (hc.trans stop_endpoint_cosine)

theorem band_rejection_power_ratio (high low : ℝ) (hlo : 1/8 ≤ high) (hhi : high ≤ 1/2)
    (llo : 0 ≤ low) (lhi : low ≤ 5/48) :
    0 ≤ cascadeResponsePower (2*Real.pi*high) / cascadeResponsePower (2*Real.pi*low) ∧
    cascadeResponsePower (2*Real.pi*high) / cascadeResponsePower (2*Real.pi*low) ≤ 1/1000000000 := by
  obtain ⟨hsn, hs⟩ := high_frequency_band_bound high hlo hhi
  have hp := (low_frequency_band_bounds low llo lhi).1
  have hp0 : 0 < cascadeResponsePower (2*Real.pi*low) := by linarith
  exact ⟨div_nonneg hsn hp0.le, (div_le_iff₀ hp0).2 (by linarith)⟩

-- Four-to-one sampling folds 47 kHz to 1 kHz at a 192 kHz engine.
theorem fortyseven_k_folds_to_one_k (n : ℕ) :
    Real.sin (2*Real.pi*47000/192000*(4*n)) = -Real.sin (2*Real.pi*1000/48000*n) := by
  have hangle : 2*Real.pi*47000/192000*(4*n) =
      (n:ℝ)*(2*Real.pi) - 2*Real.pi*1000/48000*n := by ring
  rw [hangle, Real.sin_nat_mul_two_pi_sub]
end Sheaf.Aliasing
