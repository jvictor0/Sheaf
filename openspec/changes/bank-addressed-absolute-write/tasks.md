# Tasks — bank-addressed-absolute-write

Gate: Sheaf synth test suite green, exactly at baseline (917 tests, 915
passed, 2 failed — `braid4_meets_96000hz_256_frame_deadline_and_continuity`
and `braid4_sparse_modulation_meets_96000hz_256_frame_deadline`,
deterministic pre-existing failures on this machine, untouched by this
change). New cases for the new path are added on top of that baseline; see
design.md's Testing section.

## 1. Implementation

- [x] 1.1 `Bank::HandleSetAbsoluteOnTopLevel` + private `FindTopLevelCell`,
      modeled on `Bank::HandleSetAbsolute` + `FindVisibleCell`
      (`ParameterModulation.hpp`/`.cpp`).
      Verified complete: added at `Bank::HandleSetAbsoluteOnTopLevel`
      (hpp declaration; cpp definition immediately after
      `Bank::HandleSetAbsolute`) and private `Bank::FindTopLevelCell`
      (cpp definition immediately after both `FindVisibleCell` overloads),
      each an exact structural mirror of its modeled-on counterpart, scanning
      `topLevel_` instead of `visible_`.
- [x] 1.2 `MessageIn::Type::ParamSetAbsoluteOnBank` (appended after
      `PrevParamBank` — see design.md's "Enum placement") + matching
      `MessageIn::ParamSetAbsoluteOnBank` factory, declared/defined next to
      `ParamSetAbsolute`.
      Verified complete: enumerator appended after `PrevParamBank`
      (confirmed no existing enumerator's ordinal moved — the diff touches
      no pre-existing line in the enum); factory declared/defined next to
      `ParamSetAbsolute`, built the same way, using the existing `bankIx`
      field (no new `MessageIn` field needed).
- [x] 1.3 `ParameterManager::HandleSetAbsoluteOnBank`: resolves position via
      the addressed slot, writes via `BankAt(bankIx)`'s new top-level
      primitive; no modifier gate; records the epoch on the addressed slot
      exactly as the slot-addressed path does; null-safe on both slot and
      bank.
      Verified complete: null-checks slot and bank up front (mirroring
      `SelectBankForSlot`'s own slot+bank null-check shape) and returns
      without effect if either is missing; no call to `SelectBank`, no
      write to `selectedBank_`, no call through `Owns`; no modifier check;
      `RecordProcessedAbsoluteEpoch` called on the addressed slot after a
      successful write, same as the slot-addressed path.
- [x] 1.4 `MessageInBus::Apply` dispatch case for the new type.
      Verified complete: added immediately after the `ParamSetAbsolute`
      case, guarded by `manager_ != nullptr` (matching the switch's general
      pattern rather than `ParamSetAbsolute`'s own downstream-gating
      exception, which does not apply here — see design.md).
- [x] 1.5 Build + run `cd projects/synth && nice make -j2 test`; confirm
      915/917 passed with only the two known 96kHz-deadline failures, and
      that the total stays 917 (no new tests, nothing else broke).
      Verified complete: `nice make -C projects/synth -j2 test` — 917
      total, 915 `[PASS]`, 2 `[FAIL]`
      (`braid4_meets_96000hz_256_frame_deadline_and_continuity`,
      `braid4_sparse_modulation_meets_96000hz_256_frame_deadline`), byte-
      identical to the measured baseline; zero compiler errors; `git diff
      --stat` on the two touched files shows 84 insertions, 0 deletions.

## 2. Postflight and delivery

- [ ] 2.1 Postflight against these artifacts: implementation versus
      proposal, plus a duplication pass over the whole diff.
- [ ] 2.2 Commits append to the branch this submodule is pinned to, which
      is the branch of the open upstream pull request.
