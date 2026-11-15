import Sheaf.SourceFilterCoefficients
import Mathlib.Tactic.NormNum
import Mathlib.Tactic.Linarith
import Mathlib.Tactic.Ring
import Mathlib.Tactic.Positivity
import Mathlib.Analysis.SpecificLimits.Basic

set_option exponentiation.threshold 1024

namespace Sheaf.FilterState
open Sheaf.SourceFilterCoefficients

structure State where
  recent : ℝ
  previous : ℝ

noncomputable def energy (s : Biquad) (x y : ℝ) := x^2 + s.a1*x*y + s.a2*y^2
noncomputable def next (s : Biquad) (x y disturbance : ℝ) := -s.a1*x - s.a2*y + disturbance
noncomputable def step (s : Biquad) (state : State) (disturbance : ℝ) : State :=
  ⟨next s state.recent state.previous disturbance, state.recent⟩
noncomputable def run (s : Biquad) (initial : State) (disturbance : ℕ → ℝ) : ℕ → State
  | 0 => initial
  | n+1 => step s (run s initial disturbance n) (disturbance n)

structure Certificate (s : Biquad) : Prop where
  pole_nonneg : 0 ≤ s.a2
  pole_bound : s.a2 ≤ 4/5
  feedback_nonpos : s.a1 ≤ 0
  feedback_bound : -s.a1 ≤ 2
  coercivity : 0 ≤ 4*(999/1000)*(s.a2-1/1000)-s.a1^2
  discriminant : 0 ≤ 4*s.a2-s.a1^2

-- Conditional energy certificates below are not instances for the delivered six sections.

theorem energy_coercive {s : Biquad} (h : Certificate s) (x y : ℝ) :
    (x^2+y^2)/1000 ≤ energy s x y := by
  have hs := sq_nonneg (2*(999/1000:ℝ)*x+s.a1*y)
  have hd := mul_nonneg h.coercivity (sq_nonneg y)
  unfold energy
  nlinarith

theorem energy_nonnegative {s : Biquad} (h : Certificate s) (x y : ℝ) :
    0 ≤ energy s x y := by
  have hl := energy_coercive h x y
  nlinarith [sq_nonneg x, sq_nonneg y]

theorem energy_upper {s : Biquad} (h : Certificate s) (x y : ℝ) :
    energy s x y ≤ 2*(x^2+y^2) := by
  have ha : 0 ≤ -s.a1/2 := by linarith [h.feedback_nonpos]
  have hb : 0 ≤ 1+s.a1/2 := by linarith [h.feedback_bound]
  have hc : 0 ≤ 2-s.a2+s.a1/2 := by linarith [h.feedback_bound, h.pole_bound]
  have h1 := mul_nonneg ha (sq_nonneg (x+y))
  have h2 := mul_nonneg hb (sq_nonneg x)
  have h3 := mul_nonneg hc (sq_nonneg y)
  unfold energy
  nlinarith

-- Exact identity for the implemented second-order feedback recurrence.
theorem unforced_energy_identity (s : Biquad) (x y : ℝ) :
    energy s (next s x y 0) x = s.a2 * energy s x y := by
  unfold energy next
  ring

theorem difference_is_forced_recurrence (s : Biquad) (x y xr yr d dr : ℝ) :
    next s x y d - next s xr yr dr = next s (x-xr) (y-yr) (d-dr) := by
  unfold next
  ring

theorem arithmetic_disturbance_step {s : Biquad} (h : Certificate s) (x y d : ℝ) :
    energy s (next s x y d) x ≤ (9/10)*energy s x y + 9*d^2 := by
  let u := -s.a1*x/2-s.a2*y
  have he := energy_nonnegative h x y
  have hu : u^2 ≤ s.a2*energy s x y := by
    have hd := mul_nonneg h.discriminant (sq_nonneg x)
    dsimp [u, energy]
    nlinarith
  have hp := mul_le_mul_of_nonneg_right h.pole_bound he
  have hy : 2*u*d ≤ energy s x y / 10 + 8*d^2 := by
    nlinarith [sq_nonneg (u-8*d)]
  have hid : energy s (next s x y d) x = s.a2*energy s x y + 2*u*d + d^2 := by
    dsimp [energy, next, u]
    ring
  nlinarith

theorem bounded_disturbance_step {s : Biquad} (h : Certificate s) (x y d η : ℝ)
    (hd : |d| ≤ η) :
    energy s (next s x y d) x ≤ (9/10)*energy s x y + 9*η^2 := by
  have hb := arithmetic_disturbance_step h x y d
  have he := abs_nonneg d
  have hi := abs_le.mp hd
  nlinarith

theorem unforced_transient_exact (s : Biquad) (initial : State) (n : ℕ) :
    energy s (run s initial (fun _ => 0) n).recent (run s initial (fun _ => 0) n).previous =
      s.a2^n * energy s initial.recent initial.previous := by
  induction n with
  | zero => simp [run]
  | succ n ih =>
    simp only [run, step]
    rw [unforced_energy_identity, ih, pow_succ]
    ring

theorem unforced_transient_bound {s : Biquad} (h : Certificate s) (initial : State) (n : ℕ) :
    energy s (run s initial (fun _ => 0) n).recent (run s initial (fun _ => 0) n).previous ≤
      (4/5:ℝ)^n * energy s initial.recent initial.previous := by
  rw [unforced_transient_exact]
  exact mul_le_mul_of_nonneg_right (pow_le_pow_left₀ h.pole_nonneg h.pole_bound n)
    (energy_nonnegative h _ _)

theorem accumulated_disturbance_bound {s : Biquad} (h : Certificate s) (initial : State)
    (d : ℕ → ℝ) (η : ℝ) (hd : ∀ n, |d n| ≤ η) (n : ℕ) :
    energy s (run s initial d n).recent (run s initial d n).previous ≤
      (9/10:ℝ)^n * energy s initial.recent initial.previous +
      90*η^2*(1-(9/10:ℝ)^n) := by
  induction n with
  | zero => simp [run]
  | succ n ih =>
    have hs := bounded_disturbance_step h (run s initial d n).recent
      (run s initial d n).previous (d n) η (hd n)
    simp only [run, step, pow_succ]
    nlinarith

theorem component_error_budget {s : Biquad} (h : Certificate s) (initial : State)
    (d : ℕ → ℝ) (η : ℝ) (hd : ∀ n, |d n| ≤ η) (n : ℕ) :
    (run s initial d n).recent^2 ≤
      1000*((9/10:ℝ)^n * energy s initial.recent initial.previous +
        90*η^2*(1-(9/10:ℝ)^n)) := by
  have hc := energy_coercive h (run s initial d n).recent (run s initial d n).previous
  have hb := accumulated_disturbance_bound h initial d η hd n
  nlinarith [sq_nonneg (run s initial d n).previous]

theorem transient_energy_tends_to_zero {s : Biquad} (h : Certificate s) (initial : State) :
    Filter.Tendsto (fun n => energy s (run s initial (fun _ => 0) n).recent
      (run s initial (fun _ => 0) n).previous) Filter.atTop (nhds 0) := by
  have hp : s.a2 < 1 := by linarith [h.pole_bound]
  have ht := (tendsto_pow_atTop_nhds_zero_of_lt_one h.pole_nonneg hp).mul_const
    (energy s initial.recent initial.previous)
  simpa only [unforced_transient_exact, zero_mul] using ht

theorem energy_after_512_samples {s : Biquad} (h : Certificate s) (initial : State) :
    energy s (run s initial (fun _ => 0) 512).recent
      (run s initial (fun _ => 0) 512).previous ≤
      (1/(10:ℝ)^49) * energy s initial.recent initial.previous := by
  have hp : (4/5:ℝ)^512 ≤ 1/(10:ℝ)^49 := by norm_num
  exact (unforced_transient_bound h initial 512).trans
    (mul_le_mul_of_nonneg_right hp (energy_nonnegative h _ _))

-- Full direct-form-I memory, including the two input-history copies that
-- disappear from the homogeneous error equation after two common samples.
structure Memory where
  input1 : ℝ
  input2 : ℝ
  output1 : ℝ
  output2 : ℝ

noncomputable def fullStep (s : Biquad) (m : Memory) (input residual : ℝ) : Memory :=
  ⟨input, m.input1,
    s.b0*input+s.b1*m.input1+s.b2*m.input2-s.a1*m.output1-s.a2*m.output2+residual,
    m.output1⟩

noncomputable def feedback (m : Memory) : State := ⟨m.output1,m.output2⟩

theorem full_memory_matches_feedback_step (s : Biquad) (m : Memory) (input residual : ℝ) :
    feedback (fullStep s m input residual) = step s (feedback m)
      (s.b0*input+s.b1*m.input1+s.b2*m.input2+residual) := by
  dsimp [feedback, fullStep, step, next]
  congr 1
  ring

theorem input_history_aligns_after_two_samples (s : Biquad) (a b : Memory) (u v da db ea eb : ℝ) :
    (fullStep s (fullStep s a u da) v ea).input1 = (fullStep s (fullStep s b u db) v eb).input1 ∧
    (fullStep s (fullStep s a u da) v ea).input2 = (fullStep s (fullStep s b u db) v eb).input2 :=
  ⟨rfl,rfl⟩

theorem aligned_memory_error_is_forced_recurrence (s : Biquad) (a b : Memory) (u da db : ℝ)
    (h1 : a.input1 = b.input1) (h2 : a.input2 = b.input2) :
    (fullStep s a u da).output1-(fullStep s b u db).output1 =
      next s (a.output1-b.output1) (a.output2-b.output2) (da-db) := by
  dsimp [fullStep, next]
  rw [h1,h2]
  ring

theorem zero_memory_zero_input (s : Biquad) : fullStep s ⟨0,0,0,0⟩ 0 0 = ⟨0,0,0,0⟩ := by
  simp [fullStep]

-- Once input histories agree, arbitrary common input cancels exactly from
-- the two executions' error recurrence. This covers differing initial output
-- histories, rather than assuming either execution has reached steady state.
theorem common_input_cancels (s : Biquad) (x y xr yr input error : ℝ) :
    next s x y (input+error) - next s xr yr input = next s (x-xr) (y-yr) error := by
  unfold next
  ring

theorem filter_hold_preserves_state (state : State) :
    (if false then step section0 state 0 else state) = state := by simp

end Sheaf.FilterState
