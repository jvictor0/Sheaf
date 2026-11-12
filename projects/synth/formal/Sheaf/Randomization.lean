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

-- Coverage ranges over target parameters, never source routes.
def coverageCount (targets quartile draw : Nat) : Nat := count 0 (targets + 1) quartile draw

theorem coverage_bounds (targets q r : Nat) (hq : q < 4) (hr : r < targets + 1) :
    coverageCount targets q r ≤ targets := by
  have h := count_bounds 0 (targets + 1) q r (by omega) hq hr
  unfold coverageCount
  omega

theorem eightyfour_dense_coverage (r : Nat) (hr : r < 85) :
    63 ≤ coverageCount 84 3 r ∧ coverageCount 84 3 r ≤ 84 := by
  unfold coverageCount count
  omega

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

-- Conditional source-count weights have mode two, clipped only when fewer
-- than two sources are eligible. Normalization over the finite eligible set
-- changes no relative weights. Coverage quartiles do not enter this law.
def sourceWeight (mode count : Nat) : Rat :=
  depthMagnitude (if count < mode then mode - count else count - mode)

theorem depth_positive_bounded (k : Nat) : 0 < depthMagnitude k ∧ depthMagnitude k ≤ 1 := by
  induction k with
  | zero => simp only [depthMagnitude]; grind
  | succ k ih => simp only [depthMagnitude]; grind

theorem source_mode_maximal (mode count : Nat) :
    sourceWeight mode count ≤ sourceWeight mode mode := by
  simp only [sourceWeight, Nat.lt_irrefl, ↓reduceIte, Nat.sub_self, depthMagnitude]
  exact (depth_positive_bounded _).2

theorem source_right_tail_halves (mode count : Nat) (h : mode ≤ count) :
    sourceWeight mode (count + 1) = sourceWeight mode count / 2 := by
  have h1 : ¬count < mode := by omega
  have h2 : ¬count + 1 < mode := by omega
  have hs : count + 1 - mode = (count - mode) + 1 := by omega
  simp only [sourceWeight, h1, h2, ↓reduceIte, hs, depthMagnitude]

theorem source_left_tail_halves (mode count : Nat) (h : count < mode) :
    sourceWeight mode count = sourceWeight mode (count + 1) / 2 := by
  have h1 : mode - count = (mode - (count + 1)) + 1 := by omega
  by_cases h2 : count + 1 < mode
  · simp only [sourceWeight, h, h2, ↓reduceIte, h1, depthMagnitude]
  · have he : count + 1 = mode := by omega
    simp only [sourceWeight, h, he, Nat.lt_irrefl, ↓reduceIte, Nat.sub_self, h1,
      depthMagnitude]

def sourceMode (eligible : Nat) : Nat := min 2 eligible

theorem source_mode_two (eligible : Nat) (h : 2 ≤ eligible) : sourceMode eligible = 2 := by
  unfold sourceMode
  omega

-- Selecting a distinct subset of targets is abstracted by a membership
-- predicate; unselected targets have no newly assigned source routes.
def assignedSources (selected : Bool) (draw : Nat) : Nat := if selected then draw else 0

theorem unselected_has_no_routes (draw : Nat) : assignedSources false draw = 0 := rfl

theorem selected_retains_independent_draw (draw : Nat) : assignedSources true draw = draw := rfl

end Sheaf.Randomization
