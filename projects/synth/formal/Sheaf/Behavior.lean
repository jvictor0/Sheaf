import Lean

/-! Exact rational behavioral specification, independent of the C++ evaluator.
Scene, voice and gesture counts are arbitrary. No floating-point or timing-cost
claim follows from these proofs. See requirements.json for source boundaries.
-/
namespace Sheaf

def clamp01 (x : Rat) : Rat := if x < 0 then 0 else if 1 < x then 1 else x
def Unit (x : Rat) : Prop := 0 ≤ x ∧ x ≤ 1
def blend (a b t : Rat) : Rat := a * (1 - t) + b * t

theorem clamp_bounded (x : Rat) : Unit (clamp01 x) := by
  unfold Unit clamp01
  split <;> (try split) <;> grind

theorem blend_left (a b : Rat) : blend a b 0 = a := by unfold blend; grind
theorem blend_right (a b : Rat) : blend a b 1 = b := by unfold blend; grind

structure Cell (scenes voices : Nat) where
  targets : Fin scenes → Rat
  current : Fin voices → Rat

def edit {s v : Nat} (c : Cell s v) (scene : Fin s) (target : Rat) : Cell s v :=
  { c with targets := fun i => if i = scene then clamp01 target else c.targets i }

theorem edit_isolates_current {s v : Nat} (c : Cell s v) (scene : Fin s) (x : Rat) :
    (edit c scene x).current = c.current := rfl

theorem absolute_leaf_reaches_target {s v : Nat} (c : Cell s v) (scene : Fin s) (x : Rat) :
    (edit c scene x).targets scene = clamp01 x := by simp [edit]

theorem edit_preserves_other_scene {s v : Nat} (c : Cell s v)
    (scene other : Fin s) (x : Rat) (h : other ≠ scene) :
    (edit c scene x).targets other = c.targets other := by simp [edit, h]

-- The caller supplies the post-reset scene map, including untouched scenes.
-- Sheaf resets its current center immediately, unlike Smart Grid's target ramp.
def reset {s v : Nat} (_c : Cell s v) (defaults : Fin s → Rat) (value : Rat) : Cell s v :=
  { targets := defaults, current := fun _ => clamp01 value }

theorem reset_uses_configured_defaults {s v : Nat} (c : Cell s v)
    (d : Fin s → Rat) (value : Rat) : (reset c d value).targets = d := rfl

theorem reset_snaps_current {s v : Nat} (c : Cell s v)
    (d : Fin s → Rat) (value : Rat) (voice : Fin v) :
    (reset c d value).current voice = clamp01 value := rfl

inductive BlendMode where | crossfade | attenuverter deriving DecidableEq
inductive TargetKind where | bipolar | oneWay deriving DecidableEq

def resolvedMode (mode : BlendMode) (kind : TargetKind) : BlendMode :=
  if kind = .oneWay then .crossfade else mode

def connectedDepth (kind : TargetKind) (connected : Bool) (depth : Rat) : Rat :=
  if kind = .oneWay ∧ connected = false then 0 else depth

theorem one_way_ignores_group_blend_mode (m : BlendMode) :
    resolvedMode m .oneWay = .crossfade := by simp [resolvedMode]

theorem disconnected_one_way_is_neutral (d : Rat) :
    connectedDepth .oneWay false d = 0 := by simp [connectedDepth]

def attenuverter (center depth source : Rat) (restsAtZero : Bool) : Rat :=
  center + (if restsAtZero then depth / 2 else depth) *
    (source - if restsAtZero then 0 else 1/2)

theorem resting_unipolar_preserves_center (c d : Rat) :
    attenuverter c d 0 true = c := by unfold attenuverter; grind

theorem resting_bipolar_preserves_center (c d : Rat) :
    attenuverter c d (1/2) false = c := by unfold attenuverter; grind

theorem attenuverter_zero_depth (c source : Rat) (rest : Bool) :
    attenuverter c 0 source rest = c := by cases rest <;> unfold attenuverter <;> grind

def magnitude (depth : Rat) : Rat := if depth < 0 then -depth else depth

def signedContribution (depth source : Rat) : Rat :=
  if depth < 0 then (-depth) * (1-source) else depth * source

theorem signed_contribution_matches_offset (d source : Rat) :
    signedContribution d source = d * source - (if d < 0 then d else 0) := by
  unfold signedContribution
  split <;> grind

def crossfade (center : Rat) (routes : List (Rat × Rat)) : Rat :=
  let weight := (routes.map fun p => magnitude p.1).foldl (· + ·) 0
  let denominator := if weight < 1 then 1 else weight
  let centerScale := if weight < 1 then 1-weight else 0
  center * centerScale +
    (routes.map fun p => signedContribution (p.1 / denominator) p.2).foldl (· + ·) 0

theorem empty_crossfade_preserves_center (center : Rat) : crossfade center [] = center := by
  unfold crossfade
  simp only [List.map_nil, List.foldl_nil]
  grind

-- Optional gesture slots and arbitrary-width masks are separate from scene
-- storage. Clearing an ID must leave other IDs and base scene values alone.
structure GestureTree (gestures scenes : Nat) where
  base : Fin scenes → Rat
  slots : Fin gestures → Option (Fin scenes → Rat)

def clearGesture {g s : Nat} (t : GestureTree g s) (id : Fin g) : GestureTree g s :=
  { t with slots := fun i => if i = id then none else t.slots i }

theorem clear_gesture_base {g s : Nat} (t : GestureTree g s) (id : Fin g) :
    (clearGesture t id).base = t.base := rfl

theorem clear_gesture_selected {g s : Nat} (t : GestureTree g s) (id : Fin g) :
    (clearGesture t id).slots id = none := by simp [clearGesture]

theorem clear_gesture_other {g s : Nat} (t : GestureTree g s) (id other : Fin g)
    (h : other ≠ id) : (clearGesture t id).slots other = t.slots other := by
  simp [clearGesture, h]

-- A storage slot is reclaimable only after its whole subtree is semantically
-- neutral and no modulation view pins it. The concrete predicate is native-tested.
def reclaimable (neutral : Bool) (pins : Nat) : Bool := neutral && pins == 0

theorem pinned_storage_survives (neutral : Bool) (pins : Nat) (h : 0 < pins) :
    reclaimable neutral pins = false := by
  simp [reclaimable]; omega

-- Fuego is the application's pure seam. Crunchy is shared, Crispy gets only
-- the global warp, and ordinary rows get global then localWarp warps. The numeric
-- warp functions are supplied, rather than assumed to have properties.
structure FuegoBank (rows : Nat) where
  crispy : Rat
  values : Fin rows → Rat

def fuego {n : Nat} (globalWarp localWarp : Nat → Rat → Rat → Rat)
    (crispyRow : Nat) (crunchy : Rat) (bank : FuegoBank n) : FuegoBank n :=
  { crispy := globalWarp crispyRow crunchy bank.crispy,
    values := fun i => localWarp i.val (globalWarp crispyRow crunchy bank.crispy)
      (globalWarp i.val crunchy (bank.values i)) }

theorem crispy_gets_global_only {n : Nat} (globalWarp localWarp : Nat → Rat → Rat → Rat)
    (row : Nat) (crunchy : Rat) (bank : FuegoBank n) :
    (fuego globalWarp localWarp row crunchy bank).crispy = globalWarp row crunchy bank.crispy := rfl

theorem ordinary_row_uses_warped_crispy {n : Nat} (globalWarp localWarp : Nat → Rat → Rat → Rat)
    (row : Nat) (crunchy : Rat) (bank : FuegoBank n) (i : Fin n) :
    (fuego globalWarp localWarp row crunchy bank).values i =
      localWarp i.val (globalWarp row crunchy bank.crispy) (globalWarp i.val crunchy (bank.values i)) := rfl

-- Equality retirement is an observational optimization. This does not prove
-- that C++'s bitwise quiescence predicate implies the equality premise.
def retired {α : Type} (step : α → α) (state : α) (quiet : Bool) : α :=
  if quiet then state else step state

theorem quiescence_preserves_step {α : Type} (step : α → α) (state : α)
    (quiet : Bool) (h : quiet = true → step state = state) :
    retired step state quiet = step state := by
  cases quiet <;> simp_all [retired]

-- JUDGMENT, not a theorem: which C++ fields of Parameter::ComputeAtDepth are
-- the key k, the carried state t and the derived state p. Ruled by the lead on
-- 2026-11-11 from the trace table in openspec/changes/compute-fixed-point-retirement/
-- proposal.md, and checked against the code by the native differential tests
-- (parameter_modulation_tests.cpp fixed_point_compute_retirement_matches_full_compute
-- and frogg3rs app/FroggersParameterModelTests.cpp
-- froggers_compute_retirement_matches_full_compute), never by this file.
--   K (key): everything one call reads and does not write: the clamped fresh raw
--     centre, the connected and restsAtZero flags of the active routes' sources,
--     the write epoch, and the recursionDepth and smoothTargetCenter arguments.
--   T (carried): what one call both reads and writes: targetCenter_, the route
--     structure (activeRouteCount_, routeSourceIndices_, sourceRoutePositions_)
--     and currentDepths_.
--   P (derived): everything else one call writes, a function of the key and the
--     new carried state alone (targetDepths_, target scales, offsets, minimums,
--     maximums, a depth child's cached knob and UI display state).
-- `carried` is g and `derived` is h in `step k (t, p) = (g k t, h k (g k t))`.
structure ComputeSplit (K T P : Type) where
  carried : K → T → T
  derived : K → T → P

def ComputeSplit.step {K T P : Type} (c : ComputeSplit K T P) (k : K) (s : T × P) : T × P :=
  (c.carried k s.1, c.derived k (c.carried k s.1))

-- Pins the definition of `step`: a change to it breaks this proof, and a change
-- to this statement breaks its manifest pin.
theorem compute_step_unfolds {K T P : Type} (c : ComputeSplit K T P) (k : K) (s : T × P) :
    c.step k s = (c.carried k s.1, c.derived k (c.carried k s.1)) := rfl

-- Two consecutive calls with the same key and nothing written between them, the
-- second leaving the carried state unchanged, make the second a fixed point.
-- Given the ComputeSplit judgment, no component of the derived state is compared.
-- Stated over the explicit step, so the pinned statement fixes what a call does.
theorem second_compute_fixed_point {K T P : Type} (c : ComputeSplit K T P) (k : K)
    (s0 s1 : T × P) (h1 : s1 = (c.carried k s0.1, c.derived k (c.carried k s0.1)))
    (h2 : c.carried k s1.1 = s1.1) :
    (c.carried k s1.1, c.derived k (c.carried k s1.1)) = s1 := by
  subst h1
  simp only at h2 ⊢
  rw [h2]

-- A fixed point of the step stays one, so retiring every one of n frames that
-- share the key (a retired frame leaves the state as it is) gives the state that
-- running all n steps gives. Stated over core `_root_.Nat.repeat` (a
-- `Sheaf.Nat.repeat` cannot stand in for it) and the explicit step, so the
-- pinned statement fixes both the frames and the step.
theorem retired_frames_match_full {K T P : Type} (c : ComputeSplit K T P) (k : K)
    (s : T × P) (h : (c.carried k s.1, c.derived k (c.carried k s.1)) = s) (n : Nat) :
    _root_.Nat.repeat (fun st => st) n s =
      _root_.Nat.repeat (fun x : T × P => (c.carried k x.1, c.derived k (c.carried k x.1))) n s := by
  have hl : ∀ m, _root_.Nat.repeat (fun st : T × P => st) m s = s := by
    intro m
    induction m with
    | zero => rfl
    | succ m ih => simpa [_root_.Nat.repeat] using ih
  have hr : ∀ m, _root_.Nat.repeat (fun x : T × P => (c.carried k x.1, c.derived k (c.carried k x.1))) m s = s := by
    intro m
    induction m with
    | zero => rfl
    | succ m ih =>
      simp only [_root_.Nat.repeat]
      rw [ih]
      exact h
  rw [hl, hr]

-- PRE-FILTER interpolation weights: 1/4, 1/2, 3/4, 1. The previous
-- matrix target is kept for this frame. These identities do not describe the
-- filtered/clamped DSP cache: phase four reaches the target before filtering.
def publish (previous target : Rat) (phase : Fin 4) : Rat :=
  previous + (target - previous) * ((phase.val + 1 : Nat) : Rat) / 4

theorem publication_finishes (previous target : Rat) :
    publish previous target ⟨3, by decide⟩ = target := by unfold publish; grind

theorem settled_publication (value : Rat) (phase : Fin 4) :
    publish value value phase = value := by unfold publish; grind

-- Stopping transport freezes source-filter history, while control-frame
-- evaluation remains available for edits and scene changes.
def sourceStep {State Input : Type} (advance : Bool) (filter : State → Input → State)
    (state : State) (input : Input) : State := if advance then filter state input else state

theorem stopped_sources_hold {State Input : Type} (filter : State → Input → State)
    (state : State) (input : Input) : sourceStep false filter state input = state := rfl

structure Runtime (voices : Nat) where
  previous : Fin voices → Rat
  target : Fin voices → Rat
  matrixFrames : Nat
  engineSamples : Nat

def controlFrame {v : Nat} (state : Runtime v) (evaluate seam : (Fin v → Rat) → (Fin v → Rat)) :
    Runtime v :=
  { previous := state.target, target := seam (evaluate state.target),
    matrixFrames := state.matrixFrames + 1, engineSamples := state.engineSamples + 4 }

-- Post-filter publication policy, separate from the linear response model.
-- The native binary32 tolerance 2e-5f is no greater than this rational bound.
def targetAge (age : Nat) (changed : Bool) : Nat :=
  min 4096 ((if changed then 0 else age) + 1)

def settledFiltered (raw target : Rat) (age : Nat) : Rat :=
  if 4096 ≤ age ∧ magnitude (raw-target) ≤ 1/50000 then target else raw

def rootPublication (raw target : Rat) (age : Nat) : Rat :=
  clamp01 (settledFiltered raw target age)

theorem changed_target_age_one (age : Nat) : targetAge age true = 1 := by
  simp [targetAge]

theorem no_early_settlement (raw target : Rat) (age : Nat) (h : age < 4096) :
    rootPublication raw target age = clamp01 raw := by
  simp [rootPublication, settledFiltered, Nat.not_le.mpr h]

theorem eligible_target_exact (raw target : Rat) (age : Nat)
    (ha : 4096 ≤ age) (he : magnitude (raw-target) ≤ 1/50000)
    (ht : Unit target) : rootPublication raw target age = target := by
  unfold rootPublication settledFiltered
  simp only [ha, he, and_self, if_true]
  unfold clamp01 Unit at *
  split <;> (try split) <;> grind

-- A pointwise correction bound, never a spectral attenuation theorem.
theorem root_publication_correction (raw target : Rat) (age : Nat) :
    -1/50000 ≤ rootPublication raw target age - clamp01 raw ∧
    rootPublication raw target age - clamp01 raw ≤ 1/50000 := by
  unfold rootPublication settledFiltered magnitude clamp01
  split <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;> grind

-- Fresh pre-filter matrix target; subsequent filter publication is native-tested.
theorem current_frame_uses_fresh_evaluation {v : Nat} (state : Runtime v)
    (eval seam : (Fin v → Rat) → (Fin v → Rat)) :
    (controlFrame state eval seam).target = seam (eval state.target) := rfl

theorem four_to_one_cadence {v : Nat} (state : Runtime v)
    (eval seam : (Fin v → Rat) → (Fin v → Rat))
    (h : state.engineSamples = 4 * state.matrixFrames) :
    (controlFrame state eval seam).engineSamples =
      4 * (controlFrame state eval seam).matrixFrames := by
  simp only [controlFrame]; omega

theorem engine_192k : 4 * (48000 : Nat) = 192000 := by decide

end Sheaf
