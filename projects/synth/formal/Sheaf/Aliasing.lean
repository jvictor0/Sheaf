import Sheaf.SourceFilterCoefficients
import Mathlib.Analysis.SpecialFunctions.Trigonometric.Bounds
import Mathlib.Analysis.Real.Pi.Bounds
import Mathlib.Tactic.NormNum
import Mathlib.Tactic.Linarith
import Mathlib.Tactic.Ring
import Mathlib.Tactic.Positivity

/-! Frozen linear frequency response of the implemented modulation-source filter.
Coefficients are exact real embeddings of the binary32 C++ coefficient export.
Sample-by-sample floating-point error, startup transients, nonlinear DSP, and the
separate audio output FIR are outside these theorems. -/
namespace Sheaf.Aliasing
open Sheaf.SourceFilterCoefficients

-- Squared magnitudes of B(z) and A(z) on the unit circle, c = cos ω.
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

noncomputable def d0 (s : Biquad) := 1 + s.a1^2 + s.a2^2 - 2*s.a2
noncomputable def d1 (s : Biquad) := 2*s.a1*(1+s.a2)
noncomputable def d2 (s : Biquad) := 4*s.a2
noncomputable def stopFloor (s : Biquad) := d0 s + d1 s / 25
noncomputable def passFloor (s : Biquad) := d0 s + d1 s + d2 s * (999/1000)^2
noncomputable def passCeiling (s : Biquad) := d0 s + d1 s * (999/1000) + d2 s

structure Certificate (s : Biquad) (bound : ℝ) : Prop where
  b1_eq : s.b1 = 2*s.b0
  b2_eq : s.b2 = s.b0
  d1_nonpos : d1 s ≤ 0
  d2_nonneg : 0 ≤ d2 s
  stop_pos : 0 < stopFloor s
  stop_bound : 4*s.b0^2*(26/25)^2 ≤ bound * stopFloor s
  bound_nonneg : 0 ≤ bound
  pass_pos : 0 < passFloor s
  pass_bound : (97/100)*passCeiling s ≤ 4*s.b0^2*(1999/1000)^2

theorem section0_certificate : Certificate section0 (17/1000) := by
  constructor <;> norm_num [section0, d0, d1, d2, stopFloor, passFloor, passCeiling]
theorem section1_certificate : Certificate section1 (1/50) := by
  constructor <;> norm_num [section1, d0, d1, d2, stopFloor, passFloor, passCeiling]
theorem section2_certificate : Certificate section2 (3/125) := by
  constructor <;> norm_num [section2, d0, d1, d2, stopFloor, passFloor, passCeiling]
theorem section3_certificate : Certificate section3 (3/100) := by
  constructor <;> norm_num [section3, d0, d1, d2, stopFloor, passFloor, passCeiling]

theorem numerator_lowpass {s : Biquad} {b : ℝ} (h : Certificate s b) (c : ℝ) :
    numerator s c = 4*s.b0^2*(1+c)^2 := by
  simp only [numerator, h.b1_eq, h.b2_eq]
  ring

theorem denominator_quadratic (s : Biquad) (c : ℝ) :
    denominator s c = d0 s + d1 s * c + d2 s * c^2 := by
  unfold denominator d0 d1 d2
  ring

theorem stopband_section {s : Biquad} {b c : ℝ} (h : Certificate s b)
    (lo : -1 ≤ c) (hi : c ≤ 1/25) : 0 ≤ power s c ∧ power s c ≤ b := by
  have hn := numerator_lowpass h c
  have hd := denominator_quadratic s c
  have hlin := mul_le_mul_of_nonpos_left hi h.d1_nonpos
  have hquad := mul_nonneg h.d2_nonneg (sq_nonneg c)
  have hdlo : stopFloor s ≤ denominator s c := by unfold stopFloor; nlinarith
  have hpos : 0 < denominator s c := lt_of_lt_of_le h.stop_pos hdlo
  have hs : (1+c)^2 ≤ (26/25:ℝ)^2 := by nlinarith
  have hnum := mul_le_mul_of_nonneg_left hs (show 0 ≤ 4*s.b0^2 by positivity)
  have hscale := mul_le_mul_of_nonneg_left hdlo h.bound_nonneg
  constructor
  · unfold power; rw [hn]; positivity
  · apply (div_le_iff₀ hpos).2
    nlinarith [h.stop_bound]

theorem passband_section {s : Biquad} {b c : ℝ} (h : Certificate s b)
    (lo : 999/1000 ≤ c) (hi : c ≤ 1) : 97/100 ≤ power s c := by
  have hn := numerator_lowpass h c
  have hd := denominator_quadratic s c
  have hslo : (999/1000:ℝ)^2 ≤ c^2 := by nlinarith
  have hshi : c^2 ≤ 1 := by nlinarith
  have hlinlo := mul_le_mul_of_nonpos_left hi h.d1_nonpos
  have hlinhi := mul_le_mul_of_nonpos_left lo h.d1_nonpos
  have hquadlo := mul_le_mul_of_nonneg_left hslo h.d2_nonneg
  have hquadhi := mul_le_mul_of_nonneg_left hshi h.d2_nonneg
  have hdlo : passFloor s ≤ denominator s c := by unfold passFloor; nlinarith
  have hdhi : denominator s c ≤ passCeiling s := by unfold passCeiling; nlinarith
  have hpos := lt_of_lt_of_le h.pass_pos hdlo
  have hs : (1999/1000:ℝ)^2 ≤ (1+c)^2 := by nlinarith
  have hnum := mul_le_mul_of_nonneg_left hs (show 0 ≤ 4*s.b0^2 by positivity)
  apply (le_div_iff₀ hpos).2
  nlinarith [h.pass_bound]

noncomputable def cascadePower (c : ℝ) :=
  (power section0 c * power section1 c) * (power section2 c * power section3 c)

noncomputable def cascadeResponsePower (ω : ℝ) :=
  (directPower section0 ω * directPower section1 ω) *
    (directPower section2 ω * directPower section3 ω)

theorem cascade_response_matches_polynomial (ω : ℝ) :
    cascadeResponsePower ω = cascadePower (Real.cos ω) := by
  simp only [cascadeResponsePower, direct_response_matches_polynomial, cascadePower]

theorem stopband_power_bound (c : ℝ) (lo : -1 ≤ c) (hi : c ≤ 1/25) :
    0 ≤ cascadePower c ∧ cascadePower c ≤ 1/4000000 := by
  obtain ⟨h0n, h0⟩ := stopband_section section0_certificate lo hi
  obtain ⟨h1n, h1⟩ := stopband_section section1_certificate lo hi
  obtain ⟨h2n, h2⟩ := stopband_section section2_certificate lo hi
  obtain ⟨h3n, h3⟩ := stopband_section section3_certificate lo hi
  have h01 := mul_le_mul h0 h1 h1n (by norm_num : (0:ℝ) ≤ 17/1000)
  have h23 := mul_le_mul h2 h3 h3n (by norm_num : (0:ℝ) ≤ 3/125)
  have hall := mul_le_mul h01 h23 (mul_nonneg h2n h3n) (by norm_num : (0:ℝ) ≤ 17/1000*(1/50))
  unfold cascadePower
  constructor
  · positivity
  · nlinarith

theorem passband_power_floor (c : ℝ) (lo : 999/1000 ≤ c) (hi : c ≤ 1) :
    1/2 ≤ cascadePower c := by
  have h0 := passband_section section0_certificate lo hi
  have h1 := passband_section section1_certificate lo hi
  have h2 := passband_section section2_certificate lo hi
  have h3 := passband_section section3_certificate lo hi
  have h01 := mul_le_mul h0 h1 (by norm_num : (0:ℝ) ≤ 97/100) (by linarith)
  have h23 := mul_le_mul h2 h3 (by norm_num : (0:ℝ) ≤ 97/100) (by linarith)
  have hall := mul_le_mul h01 h23 (by norm_num : (0:ℝ) ≤ (97/100)*(97/100)) (by nlinarith)
  unfold cascadePower
  nlinarith

-- 47 kHz is near 48 kHz and folds to 1 kHz after four-to-one sampling.
theorem fortyseven_k_folds_to_one_k (n : ℕ) :
    Real.sin (2*Real.pi*47000/192000*(4*n)) = -Real.sin (2*Real.pi*1000/48000*n) := by
  have hangle : 2*Real.pi*47000/192000*(4*n) =
      (n:ℝ)*(2*Real.pi) - 2*Real.pi*1000/48000*n := by ring
  rw [hangle, Real.sin_nat_mul_two_pi_sub]

theorem one_k_cosine_in_passband : 999/1000 ≤ Real.cos (2*Real.pi*1000/192000) := by
  have hp := Real.pi_pos
  have hb := Real.pi_lt_four
  have hc := Real.one_sub_sq_div_two_le_cos (x := 2*Real.pi*1000/192000)
  nlinarith [sq_nonneg (Real.pi - 2)]

theorem fortyseven_k_cosine_in_stopband : Real.cos (2*Real.pi*47000/192000) ≤ 1/25 := by
  have hangle : 2*Real.pi*47000/192000 = Real.pi/2 - Real.pi/96 := by ring
  rw [hangle, Real.cos_pi_div_two_sub]
  have h := Real.sin_le (le_of_lt (div_pos Real.pi_pos (by norm_num : (0:ℝ) < 96)))
  have hp := Real.pi_lt_d2
  linarith

theorem fortyseven_k_rejection_relative_to_one_k :
    cascadeResponsePower (2*Real.pi*47000/192000) ≤
      (1/1000000) * cascadeResponsePower (2*Real.pi*1000/192000) := by
  simp only [cascade_response_matches_polynomial]
  have hs := (stopband_power_bound _ (Real.neg_one_le_cos _) fortyseven_k_cosine_in_stopband).2
  have hp := passband_power_floor _ one_k_cosine_in_passband (Real.cos_le_one _)
  linarith

-- Continuous frequency bands, not merely a finite numerical sweep.
theorem high_frequency_band_bound (frequency : ℝ) (lo : 47000 ≤ frequency)
    (hi : frequency ≤ 96000) :
    0 ≤ cascadeResponsePower (2*Real.pi*frequency/192000) ∧
    cascadeResponsePower (2*Real.pi*frequency/192000) ≤ 1/4000000 := by
  rw [cascade_response_matches_polynomial]
  have hp := le_of_lt Real.pi_pos
  have ha : (0:ℝ) ≤ 2*Real.pi*47000/192000 := by positivity
  have hb : 2*Real.pi*frequency/192000 ≤ Real.pi := by
    nlinarith [mul_nonneg hp (show 0 ≤ 96000-frequency by linarith)]
  have hab : 2*Real.pi*47000/192000 ≤ 2*Real.pi*frequency/192000 := by
    nlinarith [mul_nonneg hp (show 0 ≤ frequency-47000 by linarith)]
  have hc := Real.cos_le_cos_of_nonneg_of_le_pi ha hb hab
  exact stopband_power_bound _ (Real.neg_one_le_cos _) (hc.trans fortyseven_k_cosine_in_stopband)

theorem low_frequency_band_floor (frequency : ℝ) (lo : 0 ≤ frequency)
    (hi : frequency ≤ 1000) : 1/2 ≤ cascadeResponsePower (2*Real.pi*frequency/192000) := by
  rw [cascade_response_matches_polynomial]
  have hp := le_of_lt Real.pi_pos
  have ha : 0 ≤ 2*Real.pi*frequency/192000 := by positivity
  have hb : 2*Real.pi*1000/192000 ≤ Real.pi := by nlinarith [Real.pi_pos]
  have hab : 2*Real.pi*frequency/192000 ≤ 2*Real.pi*1000/192000 := by
    nlinarith [mul_nonneg hp (show 0 ≤ 1000-frequency by linarith)]
  have hc := Real.cos_le_cos_of_nonneg_of_le_pi ha hb hab
  exact passband_power_floor _ (one_k_cosine_in_passband.trans hc) (Real.cos_le_one _)

theorem band_rejection_power_ratio (high low : ℝ) (hlo : 47000 ≤ high) (hhi : high ≤ 96000)
    (llo : 0 ≤ low) (lhi : low ≤ 1000) :
    0 ≤ cascadeResponsePower (2*Real.pi*high/192000) /
      cascadeResponsePower (2*Real.pi*low/192000) ∧
    cascadeResponsePower (2*Real.pi*high/192000) /
      cascadeResponsePower (2*Real.pi*low/192000) ≤ 1/1000000 := by
  obtain ⟨hsn, hs⟩ := high_frequency_band_bound high hlo hhi
  have hp := low_frequency_band_floor low llo lhi
  have hp0 : 0 < cascadeResponsePower (2*Real.pi*low/192000) := by linarith
  constructor
  · exact div_nonneg hsn (le_of_lt hp0)
  · apply (div_le_iff₀ hp0).2
    linarith

theorem band_rejection_amplitude_ratio (high low : ℝ) (hlo : 47000 ≤ high) (hhi : high ≤ 96000)
    (llo : 0 ≤ low) (lhi : low ≤ 1000) :
    Real.sqrt (cascadeResponsePower (2*Real.pi*high/192000) /
      cascadeResponsePower (2*Real.pi*low/192000)) ≤ 1/1000 := by
  apply Real.sqrt_le_iff.mpr
  constructor
  · norm_num
  · have h : (1/1000:ℝ)^2 = 1/1000000 := by norm_num
    rw [h]
    exact (band_rejection_power_ratio high low hlo hhi llo lhi).2

end Sheaf.Aliasing
