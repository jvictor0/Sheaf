import Sheaf.OutputFirCoefficients
import Sheaf.NonlinearBoundary
import Sheaf.Rounding
import Mathlib.Tactic.Positivity

set_option maxRecDepth 4096

namespace Sheaf.OutputFir

noncomputable def l1 (coefficients : List ℝ) := (coefficients.map abs).sum
noncomputable def convolve (coefficients : List ℝ) (signal : ℤ → ℝ) (n : ℤ) : ℝ :=
  match coefficients with
  | [] => 0
  | a :: rest => a*signal n + convolve rest signal (n-1)

theorem bounded_input_gain (coefficients : List ℝ) (signal : ℤ → ℝ) (B : ℝ)
    (bound : ∀ n, |signal n| ≤ B) (n : ℤ) :
    |convolve coefficients signal n| ≤ l1 coefficients*B := by
  induction coefficients generalizing n with
  | nil => simp [convolve, l1]
  | cons a rest ih =>
    calc
      |convolve (a::rest) signal n| ≤ |a*signal n|+|convolve rest signal (n-1)| := abs_add_le _ _
      _ ≤ |a| * B+l1 rest*B := by
        rw [abs_mul]
        exact add_le_add (mul_le_mul_of_nonneg_left (bound n) (abs_nonneg a)) (ih (n-1))
      _ = l1 (a::rest)*B := by simp [l1]; ring

theorem convolution_difference (coefficients : List ℝ) (x y : ℤ → ℝ) (n : ℤ) :
    convolve coefficients x n-convolve coefficients y n =
      convolve coefficients (fun k => x k-y k) n := by
  induction coefficients generalizing n with
  | nil => simp [convolve]
  | cons a rest ih =>
    simp only [convolve]
    rw [← ih]
    ring

theorem convolution_error (coefficients : List ℝ) (x y : ℤ → ℝ) (B : ℝ)
    (bound : ∀ n, |x n-y n| ≤ B) (n : ℤ) :
    |convolve coefficients x n-convolve coefficients y n| ≤ l1 coefficients*B := by
  rw [convolution_difference]
  exact bounded_input_gain coefficients _ B bound n

theorem finite_history_flush (coefficients : List ℝ) (x y : ℤ → ℝ)
    (common : ∀ n, 0 ≤ n → x n = y n) (n : ℤ) (hn : (coefficients.length:ℤ)-1 ≤ n) :
    convolve coefficients x n = convolve coefficients y n := by
  induction coefficients generalizing n with
  | nil => simp [convolve]
  | cons a rest ih =>
    have hn0 : 0 ≤ n := by simp only [List.length_cons] at hn; omega
    have ht : (rest.length:ℤ)-1 ≤ n-1 := by simp only [List.length_cons] at hn; omega
    simp only [convolve, common n hn0, ih (n-1) ht]

theorem actual_tap_count : Sheaf.OutputFirCoefficients.taps.length = 287 := by decide

set_option maxRecDepth 4096 in
set_option maxHeartbeats 2000000 in
theorem actual_l1_gain : l1 Sheaf.OutputFirCoefficients.taps ≤ 9/4 := by
  norm_num [l1, Sheaf.OutputFirCoefficients.taps]

theorem actual_history_flush (x y : ℤ → ℝ) (common : ∀ n, 0 ≤ n → x n = y n)
    (n : ℤ) (hn : 286 ≤ n) :
    convolve Sheaf.OutputFirCoefficients.taps x n = convolve Sheaf.OutputFirCoefficients.taps y n := by
  apply finite_history_flush _ x y common n
  simpa [actual_tap_count] using hn

theorem actual_final_output_error (x y : ℤ → ℝ) (B : ℝ) (hB : 0 ≤ B)
    (bound : ∀ n, |x n-y n| ≤ B) (n : ℤ) :
    |Sheaf.NonlinearBoundary.hardBound (convolve Sheaf.OutputFirCoefficients.taps x n)-
     Sheaf.NonlinearBoundary.hardBound (convolve Sheaf.OutputFirCoefficients.taps y n)| ≤ (9/4)*B := by
  exact (Sheaf.NonlinearBoundary.final_bound_does_not_amplify_error _ _).trans
    ((convolution_error _ x y B bound n).trans (mul_le_mul_of_nonneg_right actual_l1_gain hB))

-- Rounding residuals can be combined with an upstream signal error budget;
-- neither applying the FIR nor the final clamp requires upstream linearity.
theorem final_error_with_arithmetic_residual (reference ideal rounded : ℝ) (B R : ℝ)
    (upstream : |ideal-reference| ≤ (9/4)*B) (arithmetic : |rounded-ideal| ≤ R) :
    |Sheaf.NonlinearBoundary.hardBound rounded-Sheaf.NonlinearBoundary.hardBound reference| ≤ (9/4)*B+R := by
  have hclip := Sheaf.NonlinearBoundary.final_bound_does_not_amplify_error rounded reference
  have htriangle : |rounded-reference| ≤ |rounded-ideal|+|ideal-reference| := by
    simpa using abs_add_le (rounded-ideal) (ideal-reference)
  linarith

noncomputable def round32 (ftz : Bool) (x : ℝ) :=
  if ftz then Sheaf.Rounding.binary32RoundFtz x else Sheaf.Rounding.binary32Round x
noncomputable def round64 (ftz : Bool) (x : ℝ) :=
  if ftz then Sheaf.Rounding.binary64RoundFtz x else Sheaf.Rounding.binary64Round x

theorem round32_error (ftz : Bool) (x : ℝ) (hx : |x| ≤ 8) :
    |round32 ftz x-x| ≤ 1/1048576 := by
  cases ftz with
  | false =>
    have h := Sheaf.Rounding.finite_binary32_rounding_error x hx
    simpa only [round32, Bool.false_eq_true, ↓reduceIte] using h.trans (by norm_num)
  | true => exact Sheaf.Rounding.ftz_regional_rounding_bound x hx

theorem round64_error (ftz : Bool) (x : ℝ) (hx : |x| ≤ 16) :
    |round64 ftz x-x| ≤ 1/281474976710656 := by
  cases ftz with
  | false =>
    have h := Sheaf.Rounding.finite_binary64_rounding_error x hx
    simpa only [round64, Bool.false_eq_true, ↓reduceIte] using h.trans (by norm_num)
  | true => exact Sheaf.Rounding.ftz_binary64_regional_bound x hx

-- The shipping FIR rounds each product to binary32, adds it to a binary64
-- accumulator, and rounds the final output to binary32. These definitions
-- model that mixed-precision path, including optional flush-to-zero.
noncomputable def roundedSum (ftz : Bool) (terms : List ℝ) (accumulator : ℝ) : ℝ :=
  match terms with
  | [] => accumulator
  | a :: rest => roundedSum ftz rest ((round64 ftz) (accumulator+a))

noncomputable def safeSum (ftz : Bool) (terms : List ℝ) (accumulator : ℝ) : Prop :=
  match terms with
  | [] => True
  | a :: rest => |accumulator+a| ≤ 16 ∧
      safeSum ftz rest ((round64 ftz) (accumulator+a))

theorem accumulated_binary64_error (ftz : Bool) (terms : List ℝ) (accumulator : ℝ)
    (safe : safeSum ftz terms accumulator) :
    |roundedSum ftz terms accumulator-(accumulator+terms.sum)| ≤ terms.length/281474976710656 := by
  induction terms generalizing accumulator with
  | nil => simp [roundedSum]
  | cons a rest ih =>
    have hr := round64_error ftz (accumulator+a) safe.1
    have ht := ih ((round64 ftz) (accumulator+a)) safe.2
    have he : roundedSum ftz (a::rest) accumulator-(accumulator+(a::rest).sum) =
        (roundedSum ftz rest ((round64 ftz) (accumulator+a))-
          ((round64 ftz) (accumulator+a)+rest.sum)) +
        ((round64 ftz) (accumulator+a)-(accumulator+a)) := by
      simp only [roundedSum, List.sum_cons]
      ring
    rw [he]
    have hb := (abs_add_le _ _).trans (add_le_add ht hr)
    simp only [List.length_cons, Nat.cast_add, Nat.cast_one]
    linarith

theorem summed_binary32_product_error (ftz : Bool) (terms : List ℝ) (safe : ∀ a ∈ terms, |a| ≤ 8) :
    |(terms.map (round32 ftz)).sum-terms.sum| ≤ terms.length/1048576 := by
  induction terms with
  | nil => simp
  | cons a rest ih =>
    have hr := round32_error ftz a (safe a (by simp))
    have ht := ih (fun b hb => safe b (by simp [hb]))
    have he : ((a::rest).map (round32 ftz)).sum-(a::rest).sum =
        ((round32 ftz) a-a)+
        ((rest.map (round32 ftz)).sum-rest.sum) := by simp; ring
    rw [he]
    have hb := (abs_add_le _ _).trans (add_le_add hr ht)
    simp only [List.length_cons, Nat.cast_add, Nat.cast_one]
    linarith

noncomputable def mixedPrecisionFirSum (ftz : Bool) (terms : List ℝ) :=
  (round32 ftz) (roundedSum ftz (terms.map (round32 ftz)) 0)

theorem mixed_precision_output_error (ftz : Bool) (terms : List ℝ)
    (products : ∀ a ∈ terms, |a| ≤ 8)
    (accumulators : safeSum ftz (terms.map (round32 ftz)) 0)
    (output : |roundedSum ftz (terms.map (round32 ftz)) 0| ≤ 8) :
    |mixedPrecisionFirSum ftz terms-terms.sum| ≤
      (terms.length+1)/1048576 + terms.length/281474976710656 := by
  have hp := summed_binary32_product_error ftz terms products
  have ha := accumulated_binary64_error ftz (terms.map (round32 ftz)) 0 accumulators
  have ho := round32_error ftz _ output
  simp only [zero_add, List.length_map] at ha
  have h1 := abs_add_le
    ((round32 ftz) (roundedSum ftz (terms.map (round32 ftz)) 0)-
      roundedSum ftz (terms.map (round32 ftz)) 0)
    (roundedSum ftz (terms.map (round32 ftz)) 0-
      (terms.map (round32 ftz)).sum)
  have h2 := abs_add_le
    ((round32 ftz) (roundedSum ftz (terms.map (round32 ftz)) 0)-
      (terms.map (round32 ftz)).sum)
    ((terms.map (round32 ftz)).sum-terms.sum)
  simp only [sub_add_sub_cancel] at h1 h2
  unfold mixedPrecisionFirSum
  linarith

theorem actual_287_tap_arithmetic_budget (ftz : Bool) (terms : List ℝ) (hlen : terms.length = 287)
    (products : ∀ a ∈ terms, |a| ≤ 8)
    (accumulators : safeSum ftz (terms.map (round32 ftz)) 0)
    (output : |roundedSum ftz (terms.map (round32 ftz)) 0| ≤ 8) :
    |mixedPrecisionFirSum ftz terms-terms.sum| ≤ 1/3000 := by
  have h := mixed_precision_output_error ftz terms products accumulators output
  rw [hlen] at h
  exact h.trans (by norm_num)

theorem daz_input_tail_error_bound (signal : ℤ → ℝ) (n : ℤ) :
    |Sheaf.NonlinearBoundary.hardBound (convolve Sheaf.OutputFirCoefficients.taps
      (fun k => Sheaf.Rounding.flushSubnormal (signal k)) n)-
      Sheaf.NonlinearBoundary.hardBound (convolve Sheaf.OutputFirCoefficients.taps signal n)| ≤
      (9/4)*Sheaf.Rounding.smallestNormal := by
  apply actual_final_output_error _ _ Sheaf.Rounding.smallestNormal
    (by unfold Sheaf.Rounding.smallestNormal; positivity) _ n
  intro k
  exact Sheaf.Rounding.flush_error_bound (signal k)

end Sheaf.OutputFir
