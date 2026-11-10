import Sheaf.Behavior

/-! Exact count mapping and rational depth law. Uniform independent inputs are
assumptions; PRNG quality, float curve inversion, gesture dispatch and storage
lifecycle remain native-tested. No tail truncation is introduced here. -/
namespace Sheaf.Randomization

def count (minimum range quartile draw : Nat) : Nat :=
  minimum + (quartile * range + draw) / 4

theorem count_bounds (m k q r : Nat) (_hk : 0 < k) (hq : q < 4) (hr : r < k) :
    m ≤ count m k q r ∧ count m k q r < m + k := by
  have hmul : q * k ≤ 3 * k := Nat.mul_le_mul_right k (by omega)
  unfold count
  omega

-- Every count has exactly four consecutive preimages in the flattened
-- uniform q,r rectangle. These lemmas establish the marginal uniform law.
theorem count_fiber (n c : Nat) :
    n / 4 = c ↔ 4 * c ≤ n ∧ n < 4 * c + 4 := by omega

theorem rectangle_roundtrip (k n : Nat) :
    (n / k) * k + n % k = n := by
  simpa [Nat.mul_comm, Nat.add_comm] using Nat.mod_add_div n k

theorem flattened_count (m k n : Nat) :
    count m k (n / k) (n % k) = m + n / 4 := by
  unfold count
  rw [rectangle_roundtrip]

theorem rectangle_quartile (k n : Nat) (hk : 0 < k) (hn : n < 4 * k) :
    n / k < 4 ∧ n % k < k := by
  exact ⟨(Nat.div_lt_iff_lt_mul hk).mpr hn, Nat.mod_lt n hk⟩

theorem fifteen_source_bands (q r : Nat) :
    count 0 16 q r = 4 * q + r / 4 := by
  unfold count
  omega

def eligibleCount (eligible minimum quartile draw : Nat) : Nat :=
  if eligible = 0 then 0 else
    count (min minimum eligible) (eligible - min minimum eligible + 1) quartile draw

theorem no_sources_no_routes (m q r : Nat) : eligibleCount 0 m q r = 0 := by
  simp [eligibleCount]

theorem drilled_count_floor (n q r : Nat) (hn : 0 < n) :
    1 ≤ eligibleCount n 1 q r := by
  have h : min 1 n = 1 := by omega
  simp [eligibleCount, Nat.ne_of_gt hn, h, count]

def depthMagnitude : Nat → Rat
  | 0 => 1
  | k + 1 => depthMagnitude k / 2

def geometricMass (k : Nat) : Rat := depthMagnitude (k + 1)

theorem depth_starts_full : depthMagnitude 0 = 1 := rfl

theorem depth_halves (k : Nat) : depthMagnitude (k + 1) = depthMagnitude k / 2 := rfl

theorem geometric_partial_mass (n : Nat) :
    (List.range n |>.map geometricMass).sum = 1 - depthMagnitude n := by
  induction n with
  | zero => simp [depthMagnitude]; grind
  | succ n ih =>
    simp only [List.range_succ, List.map_append, List.sum_append, List.map_cons,
      List.map_nil, List.sum_cons, List.sum_nil, ih, geometricMass,
      depthMagnitude]
    grind

def requestedDepth (kind : Sheaf.TargetKind) (negative : Bool) (k : Nat) : Rat :=
  if kind = .bipolar ∧ negative then -depthMagnitude k else depthMagnitude k

theorem one_way_positive_sign (negative : Bool) (k : Nat) :
    requestedDepth .oneWay negative k = depthMagnitude k := by simp [requestedDepth]

def pressCounts (q : Fin 4) (targets : List (Nat × Nat × Nat)) : List Nat :=
  targets.map fun (m, k, r) => count m k q.val r

theorem press_shares_quartile (q : Fin 4) (m k r : Nat) (rest : List (Nat × Nat × Nat)) :
    pressCounts q ((m, k, r) :: rest) = count m k q.val r :: pressCounts q rest := rfl

end Sheaf.Randomization
