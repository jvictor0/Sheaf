# Spec Coverage

Last audit: synth audio input (JUCE/browser input views, deterministic rig injection, browser capture privacy, real-Wasm input coverage, and zero-input Mini App/Braid 4 smoke coverage), 2026-08-02.

Prior audits: portable UI component library, 2026-07-31; controller configuration wizard, 2026-07-26.

| Requirement | Status | Primary exact coverage |
|---|---|---|
| `sprs-1` | covered | `runtime_main_component_tests`, `browser_runtime_contract_tests`, `runtime_shell_session_tests`, fake-app/miniapp Playwright, generic-runtime scan |
| `sprs-2` (modified) | covered | shell composition-contract and root-validation cases in `runtime_main_component_tests`, `RuntimeShellSessionTests` additive-width and unclipped-draw cases, `places nested sidebar descendants from parent-relative bounds` |
| `sprs-3` | covered | browser service/audio/MIDI contract tests, retained JUCE runtime-page executables, browser page Playwright |
| `sprs-4` | covered | browser pointer backend tests, fake-app and miniapp real-mouse Playwright, JUCE parity executable |
| `sprs-5` | covered | browser isolated rounded-arc test, encoder geometry executable, real-miniapp Canvas/screenshots |
| `sprs-6` (modified) | covered | `ui-backend.spec.ts` parent-relative placement, no-rescue, resolved-host-height and surface-scale cases plus fake-app and miniapp desktop/narrow Playwright |
| `sprs-7` | covered | C++, JUCE, TypeScript/Chromium, real-WASM audio/MIDI/gesture/static-site acceptance |
| `sprs-9` (modified) | covered | `PortableJuceBackendTests` semantic-host, ancestor-fold, no-rescue, node-local draw and retained-editor cases |
| `sprs-10` | covered | generic JUCE viewport/content extent, two-axis reachability, retained position, and clamp cases |
| `sprs-11` | covered | real runtime-shell Controllers navigation/reachability plus seeded generic Controllers simulation and harness build |
| `sru-1` (modified) | covered | shared component navigation, JUCE shared renderer, browser page replacement/restore and narrow layout |
| `sar-10` (modified) | covered | shared host routing/refresh tests, JUCE shell session, browser worker/static typed-entry acceptance |
| `spv-1` | covered | `projects/synth/tests/portable_ui_tests.cpp` visualizer contract checks: bounds, default visible, hide/show, non-copyable/non-movable, JUCE-free compile |
| `spv-2` | covered | `projects/synth/tests/miniapp_system_tests.cpp` distinct MiniApp VCO visualizer addresses |
| `spv-3` | covered | `projects/synth/tests/portable_ui_tests.cpp` scope visualizer reads updated atomic model state without reconstruction; MiniApp retains app-owned instances |
| `spv-4` | covered | `projects/synth/tests/portable_ui_tests.cpp` `Builder::Visualizer` emits visible node and omits hidden node |
| `spv-5` | covered | `projects/synth/tests/portable_ui_tests.cpp` scope visualizer snapshots connected/color/scope/channel fields and keeps waveform geometry in bounds |
| `spv-6` | covered | `portable_ui_tests` predictive round geometry and invalid-snapshot fallback; `miniapp_juce_backend_parity_tests` and `browser_command_buffer_tests` existing-backend command parity |
| `spv-7` | covered | `TestNoiseWaveformVisualizer` in `portable_ui_tests`; `TestStandardModulatorVisualizersRemainPortable` and `miniapp_registers_noise_at_standard_index_fourteen` cover the retained standard noise visualizer at index `14` |
| `spm-81` | covered | `projects/synth/tests/parameter_modulation_tests.cpp` visualizer topology flows metadata -> depth config -> UI state, clears on disconnect, and stays out of JSON |
| `spm-89` | covered | typed controller address matching, note press/release including raw zero-velocity note-on and nonzero-velocity note-off, persistence compatibility, and note-feedback suppression in `parameter_modulation_tests` |
| `smi-9` | covered | per-section and per-kind address-type acceptance/rejection in `instrument_tests` |
| `sru-24` | covered | `projects/synth/tests/miniapp_system_tests.cpp` visualizer node shares encoder bounds, precedes encoder, and encoder actions remain; top-level/bank-transition no-visualizer regressions; null/hidden paths in portable/Braid tests |
| `sru-25` | covered | `projects/synth/tests/portable_ui_tests.cpp` shared encoder underlay body alpha and preserved non-body commands; `projects/synth/tests/miniapp_system_tests.cpp` visible and hidden visualizer underlay wiring |
| `sru-27` | covered | type-aware block round trips in `blocks_tests`, row editing in `viewmodel_tests`, and portable combo rendering/dispatch in `controllers_page_ui_tests` |
| `sdsp-13` (modified) | covered | `projects/synth/tests/dsp_tests.cpp` deterministic seeded noise, strict open interval, one advance per voice, distribution sanity, stable pointers, and direct `ParameterGroup` publication; `miniapp_registers_noise_at_standard_index_fourteen` covers application adoption |
| `sdsp-6` (modified) | covered | `one_pole_filters_and_tanh_follow_dsp_contract` covers cutoff/alpha equivalence, shared-alpha independent states, and deterministic reset seeding |
| `sdsp-33` (modified) | covered | `miniapp_registers_distinct_scope_visualizers_for_modulators` proves three distinct retained scope visualizers at application indexes `4/5/6`; `miniapp_registers_standard_fifteen_source_topology_without_changing_performer_topology` separates standard visualizers at `0..3/11/14` |
| `sdsp-34` | covered | `dsp_tests` shaped interpolation, reciprocal-time correlated increments, Hz-domain voice spread, validation, precision, and one-hour increment floor cases |
| `sdsp-35` | covered | `dsp_tests` deterministic voice wait/move/done transitions, exact and overshot boundaries, reset semantics, and double progress cases |
| `sdsp-36` | covered | `dsp_tests` canonical random draw order, correlated gang turnover, fixed storage/seed, bounded coherent snapshots, complete live fields, and assigned voice colors |
| `sdsp-37` | covered | `projects/synth/tests/dsp_tests.cpp` positive runtime voice count, zero rejection, non-copyable/non-movable lifetime, bounds-checked access, stable source pointers, and allocation-free/noexcept processing contract |
| `sdsp-38` | covered | `miniapp_registers_noise_at_standard_index_fourteen`, `miniapp_publishes_new_noise_values_before_each_modulation_update`, and `TestStandardModulatorVisualizersRemainPortable` |
| `spv-8` | covered | `projects/synth/tests/portable_ui_tests.cpp` ordered centered half-width constant bars, exact zero/top framing, invalid bounds, immutable redraw, constant-only frame preference, and builder composition; `projects/synth/tests/miniapp_system_tests.cpp` constant-frame suppression with default-frame preservation; existing JUCE/browser fill-command parity |
| `sdsp-39` | covered | `projects/synth/tests/dsp_tests.cpp` zero/one voice construction, exact even/odd greedy assignments, normalized rank coverage, maximal cyclic distance, immutable stable pointers, and direct `ParameterGroup` publication |
| `sdsp-40` | covered | `miniapp_registers_constant_at_standard_index_eleven_without_sample_work`, `miniapp_registers_standard_fifteen_source_topology_without_changing_performer_topology`, and `TestStandardModulatorVisualizersRemainPortable` |
| `ssm-1` | covered | `standard_modulators_owns_address_stable_source_and_visualizer_storage` plus compile-time copy/move assertions in `dsp_tests` |
| `ssm-2` | covered | `standard_modulators_defaults_match_min16_contract`, `standard_modulators_pre_registration_overrides_are_registered`, and `standard_modulators_configuration_freezes_after_registration` |
| `ssm-3` | covered | `standard_modulators_defaults_match_min16_contract` checks all four sources use unchanged means/target sigmas, tripled waiting/moving external sigmas, and doubled waiting/moving internal sigmas |
| `ssm-4` | covered | the `standard_modulators_rejects_*` atomic-validation cases and `standard_modulators_mono_omits_constant_and_ignores_constant_collision` |
| `ssm-5` | covered | `standard_modulators_lifecycle_requires_registration_and_finite_preparation`, `standard_modulators_process_advances_dynamic_sources_once_and_copies_voice_order`, and `standard_modulators_group_updates_and_ui_publication_remain_explicit` |
| `spm-71` | covered | MiniApp topology/system coverage plus two-scope main-screen and complete 4x4 encoder-grid parity across portable trees, browser serialization, shared geometry, and JUCE |
| `d4-1` (modified) | covered | `initializes_parameter_groups_banks_slot_and_scene_endpoints`, `braid4_filter_storage_seeds_all_eighty_owned_caches_and_excludes_xy_and_nested_depths`, `braid4_standard_bundles_register_exact_independent_sources`, and `braid4_groups_fit_sparse_fifteen_position_modulation_views` |
| `d4-2` (modified) | covered | `braid_vco_registers_three_group_shapes_two_scenes_fourteen_red_parameters_and_sparse_bank`, `braid_vco_maps_all_parameter_ranges_to_natural_vco_inputs`, and `braid_and_matrix_banks_expose_required_encoder_cells` |
| `d4-3` | covered | `prepares_four_x_internal_rate_and_sequences_internal_subframes`, `matrix_feedback_uses_current_vco_outputs_and_delays_only_modulator_consumption_one_internal_sample`, and `braid4_deadline_tests` |
| `d4-7` (modified) | covered | `prepares_four_x_internal_rate_and_sequences_internal_subframes`, `braid4_owned_caches_share_row_cutoff_alpha_but_keep_independent_filter_state`, and all three `braid4_meets_*_256_frame_deadline_and_continuity` cases |
| `d4-8` (modified) | covered | `parallel_lfo_topology_banks_colors_and_modulator_slots`, `braid_vco_supports_frequency_octave_shift_and_parameter_colors`, `audio_and_lfo_outputs_publish_normalized_stereo_mono_and_quad_modulators`, and the matrix-delay case named for `d4-3` |
| `d4-10` | covered | `braid4_filter_storage_seeds_all_eighty_owned_caches_and_excludes_xy_and_nested_depths`, `braid4_owned_caches_share_row_cutoff_alpha_but_keep_independent_filter_state`, phase/order system cases, and `braid4_deadline_tests` |
| `d4-9` | covered | `braid4_standard_modulation_view_renders_underlay_and_app_sources_remain_encoder_only`, `braid4_standard_bundles_register_exact_independent_sources`, and `TestBraid4StandardModulationViewsRemainPortable` |
| `sar-7` (modified) | covered | `concurrent_absolute_alert_and_position_output_linearize_before_alert_or_after_acknowledgement`, `rig_absolute_feedback_follows_real_acknowledgement_and_ignores_modulation`, `rig_absolute_feedback_resolves_only_latest_same_route_rapid_input`, `rig_generic_absolute_feedback_round_trips_same_address_without_auxiliary_traffic`, and `engine_rebuild_retains_pending_absolute_feedback_across_bank_route_change` |
| `spm-20` (modified) | covered | `parameter_ui_snapshot_owns_parameter_source_and_gesture_colors`, `ui_state_reports_affecting_masks_through_gesture_index_63`, `ui_state_publishes_normalized_raw_center_before_modulation_smoothing_and_presentation`, `raw_center_and_processed_epoch_require_one_stable_revision`, `processed_absolute_epoch_follows_slot_position_across_bank_and_modulation_view_changes`, `randomized_message_bus_ui_state_simulation`, and portable encoder snapshot/render assertions through bit 63 |
| `spm-35` (modified) | covered | `twister_output_debounces_reset_and_uses_channels`, `twister_output_uses_full_brightness_for_connected_cells`, `twister_color_helper_matches_smart_grid_hue_shape`, and `absolute_encoder_output_gates_until_acknowledged_and_suppresses_exact_echo_for_both_protocols` |
| `spm-62` (modified) | covered | `mf_twister_default_profile_maps_encoders_and_input_only_side_buttons`, `midi_controller_profile_threads_absolute_route_identity_and_mode`, and `midi_controller_profile_keeps_relative_and_output_only_feedback_uncoordinated` |
| `spm-68` (modified) | covered | `twister_output_blanks_disconnected_mapped_cells_with_brightness_off_values_once`, `twister_output_blanks_mapped_encoder_beyond_visible_cell_capacity_with_brightness_off_values_once`, `twister_output_ignores_unmapped_encoder_without_blanking`, and `tracked_absolute_output_beyond_visible_capacity_stays_safely_gated_while_pending` |
| `spm-78` | covered | `midi_encoder_input_absolute_publishes_matching_epoch_and_rolls_back_rejected_push`, `param_set_absolute_acknowledges_apply_modifier_rejection_and_disconnected_routes_monotonically`, absolute encoder acknowledgement/output cases, snapshot-only grid lookup, and existing output-byte/cache/reset/color/budget regressions |
| `spm-79` | covered | `generic_controller_profile_derives_same_address_position_only_output_and_honors_overrides`, `generic_absolute_encoder_output_uses_same_address_causal_acknowledgement`, `generic_absolute_encoder_output_retries_failed_correction_without_state_mutation`, `generic_relative_encoder_output_uses_display_value_without_epoch_coordination`, `generic_untracked_absolute_encoder_output_uses_raw_center_fallback`, `rig_generic_absolute_feedback_round_trips_same_address_without_auxiliary_traffic`, and `engine_rebuild_retains_pending_absolute_feedback_across_bank_route_change` |
| `spm-25` (modified) | covered | `randomized_message_bus_ui_state_simulation` plus `message_bus_sparse_lifecycle_model_tracks_pins_collection_reuse_and_patch_load` validate 64-bit UI masks and sparse lifecycle state against deterministic manager-owned oracles |
| `spm-11` (modified) | covered | `process_lite_phases_replace_cached_knob_before_ui_smoothing`, `process_lite_wrapper_matches_explicit_phases`, `parameter_process_sample_phases_recompute_only_in_phase_one`, and `group_process_sample_phases_visit_only_registered_roots` |
| `spm-66` (modified) | covered | `process_lite_phases_replace_cached_knob_before_ui_smoothing` and `ui_display_center_and_spread_follow_cached_knob_order` |
| `spm-72` | covered | `group_process_sample_visits_only_registered_roots`, `group_process_sample_phases_visit_only_registered_roots`, `recursive_local_compute_seeds_display_without_audio_rate_processing`, active-route full-scan cases, and `braid4_sparse_work_counters_bound_inactive_capacity` |
| `smod-9` (modified) | covered | `braid_vco_registers_three_group_shapes_two_scenes_fourteen_red_parameters_and_sparse_bank` and `braid_vco_maps_all_parameter_ranges_to_natural_vco_inputs` |
| `smod-10` (modified) | covered | `bipolar_matrix_registers_row_major_identity_parameters_and_bank_cells` and `braid4_owned_caches_share_row_cutoff_alpha_but_keep_independent_filter_state` |
| `smod-11` (modified) | covered | `braid_vco_supports_frequency_octave_shift_and_parameter_colors` and `parallel_lfo_topology_banks_colors_and_modulator_slots` |
| `spm-73` | covered | 0--64 gesture boundary/sparse-mask tests, `ControllerGesture63SelectsAndEditsManagerGestureWhileBankMaskRemains32Bit`, portable bit-63 badge rendering, and randomized UI-state coverage |
| `spm-74` | covered | neutral-leaf guard, bottom-up collapse, pin, settling/detach, bounded-reuse, patch-load, semantic-JSON, and randomized lifecycle cases in `parameter_modulation_tests` |
| `spm-75` | covered | connected-only modulation-view materialization/capacity/randomization cases in `parameter_modulation_tests`, MiniApp and Braid4 sparse-position system assertions, `TestBraid4StandardModulationViewsRemainPortable`, plus grid message factories/JSON validation and mixed FIFO routing/namespace isolation |
| `spm-31` (modified) | covered | relative decoder regressions plus `midi_encoder_input_absolute_maps_raw_positions_independent_of_turn_step` and absolute mapped/push/thru boundaries |
| `spm-52` (modified) | covered | encoder-mode contract/migration JSON tests, `ParamSetAbsolute` message/association round trips, and existing profile/factory coverage |
| `spm-76` | covered | pure coefficient/projection tests, focused handler cases, `handle_set_absolute_seeded_property_matches_independent_post_arming_model`, and exact polyphonic-pressure recognition/thru-once behavior |
| `spm-77` | covered | six `param_set_absolute_*` construction, association, visible-cell routing, modifier, and no-op boundary tests, plus schema-1 reads, schema-2 writes, and signed grid round trips |
| `sru-26` | covered | `EncoderModeCatalogExposesAllChoicesInDeclarationOrder`, absolute edit-session/live-rebuild tests, portable Controllers action coverage, and paired grid expansion/reconstruction with atomic hidden-orphan sessions |
| `bgr-1` | covered | `button_grid_tests` checked signed half-open geometry, row-major flattening, independent equal-range slots; `parameter_modulation_tests` parameter/grid namespace isolation |
| `bgr-2` | covered | `button_grid_tests` exact-range selection, atomic mismatch, duplicate registration, ordered callbacks, and invalid/empty/disconnected no-ops |
| `bgr-3` | covered | `button_grid_tests` all `StateCell` modes, flash policies, pressure no-op, and non-owned state lifetime |
| `bgr-4` | covered | `button_grid_tests` atomic `(r,g,b,0|1)` publication, empty/disconnected clearing, negative lookup, and stale clearing after grid switch |
| `bgr-5` | covered | `button_grid_tests` topology freeze plus unchanged capacities, storage addresses, and stable grid/slot pointers across runtime operations |
| `sar-24` | covered | `engine_tests` manager/facade lifetime, pre-processor finalization, dual-bus pump, and publication; `rig_tests` and `miniapp_system_tests` unchanged app contract without grids |
| `sru-27` | covered | JUCE-free block/view-model/portable binaries plus generic-JUCE portable backend/runtime-shell coverage and `controllers_page_simulation_tests` independent seeded oracle |
| `sdsp-42` | covered | six `phasor2tick_*` cases in `dsp_tests`: silent priming/same-cell work, exact floor-cell crossing, backward/jump detection, invalid input, and allocation-free processing |
| `smc-1` | covered | `engine_owns_one_stable_clock_prepares_before_app_and_publishes_exact_current_plan`, `master_clock_default_and_prepare_state_match_the_output_domain_contract`, `acceptance_trace_internal_timeline_orders_half_open_fractional_transport_epochs` |
| `smc-2` | covered | both `clock_plan_queries_*` cases, `master_clock_commits_exact_adjacent_anchors_and_only_future_tempo_slopes`, internal-timeline trace, `clock_plan_is_queried_at_every_fractional_oversampled_subframe` |
| `smc-3` | covered | `master_clock_rejects_invalid_prepare_and_tempo_transactionally`, `master_clock_receive_authority_rejects_manual_tempo_and_restores_it`, MiniApp tempo-authority cases |
| `smc-4` | covered | internal-timeline and external-acquisition traces plus `master_clock_internal_transport_uses_boundary_epochs_and_current_run_time` and `master_clock_external_start_arms_and_first_clock_is_timestamped_zero` |
| `smc-5` | covered | `master_clock_receive_gating_and_send_policy_are_independent`, `master_clock_transport_only_external_input_keeps_internal_tempo_authority`, `master_clock_ppqn_change_clears_external_lock_without_changing_lifetime_or_bpm` |
| `smc-6` | covered | `acceptance_trace_external_acquisition_jitter_dropout_takeover_and_regeneration`, source-arbitration cases, delayed timestamp-history case, and Engine timestamp-order merge cases |
| `smc-7` | covered | external-acquisition trace (64 stable intervals, jitter, missed clocks, dropout) plus focused PLL, phase-correction, and reacquisition cases |
| `smc-8` | covered | internal-timeline and external-acquisition traces, `master_clock_external_activation_and_stop_fill_output_only_splices`, and cross-sender regeneration cases |
| `smc-9` | covered | `acceptance_trace_mapper_output_calculation_reports_normative_maxima`, `acceptance_trace_concrete_sender_broadcast_reconnect_cutoff_and_fallback`, and focused mapper/sender generation, cutoff, lateness, half-open, reconnect, and overflow cases |
| `smc-10` | covered | runtime-config v1/v2 migration, exact sync-field round trip, atomic rejection, and patch-exclusion cases in `parameter_modulation_tests` plus host Sync persistence tests |
| `smc-11` | covered | `engine_clock_diagnostics_publication_never_tears_known_tuples`, `engine_publishes_sensible_clock_diagnostics_before_and_after_audio`, Sync page portable/JUCE/browser status coverage |
| `smi-10` | covered | `MessageInRealtimeFactoriesPreserveInternalDefaultsAndExternalIdentity`, `RealtimeMidiProcessorTranslatesExactSingleByteMessagesWithOriginalTimestampAndSlot`, `EveryControllerProfileEndsInRealtimeMidiIncludingEmptyProfiles`, Engine ordered merge, empty-profile Rig case |
| `smi-11` | covered | concrete-sender acceptance trace plus `scheduled_realtime_broadcast_preserves_original_deadline`, equal-deadline ordering, generation cutoff, overflow, and per-sink feedback isolation cases |
| `smi-12` | covered | host-lead and immediate-fallback sender cases, browser timestamp-epoch and scheduled-Web-MIDI tests, and `portable_draw_geometry_tests` JUCE scheduling-capability/epoch/deadline assertions |
| `sar-3` (modified) | covered | Engine stable-clock/prepare/current-plan cases and `rig_exposes_deterministic_clock_injection_queries_and_scheduled_output` |
| `sar-6` (modified) | covered | Engine timestamp-order/commit/delegation cases, allocation-free block path, internal-timeline trace, Braid fractional 4x query case, and input-clamped block delegation in `audio_input_tests` / `browser_runtime_contract_tests` |
| `sar-11` (modified) | covered | MiniApp clocked ADSR topology, exact nondivisor-block gate boundaries, current-frame voice publication, tempo authority, and Rig clock surface cases |
| `sar-18` (modified) | covered | Engine load-before-rebuild/default/save cases; runtime-config migration/atomic-save/patch-exclusion cases; JUCE/browser Sync Back persistence |
| `sar-30` (modified) | covered | `browser_audio_device_tests` System Default input/output catalog cases, JUCE native enumeration retention, and Playwright Audio page zero-input/input-capable assertions |
| `sar-31` | covered | `audio_input_tests` input-view preconditions, callback-lifetime rules, safe silence, null-channel and shortfall cases; `browser_runtime_contract_tests` browser limit/ABI cases; `tests/audio-input.spec.ts` real-Wasm literal-zero and shortfall probes |
| `sar-32` | covered | `audio_input_tests` `SynthRig` deterministic channel/frame/block injection, invalid-shape rejection, silence-before-injection, no-stale-input cases, and allocation probe showing injection adds no allocations beyond the rig's existing capture vector path |
| `sru-2` (modified) | covered | `TestSidebarOpensEachPageAndBackRestoresApp`, `TestRefreshUpdatesRuntimePageModelsAndRollingDeadline`, `TestSidebarWarningReflectsControllersDiscoverySnapshot`, `TestWizardDiscoveryCacheRecomputesOnlyForCachedSnapshotChanges`, `TestBrowserControllerDiscoveryCacheUsesSignalsAndSuccessfulCommits`, JUCE sidebar warning marker, browser navigation |
| `sru-3` (modified) | covered | `portable_ui_tests` Audio selector/status surface cases, `browser_audio_device_tests` host catalog/status/retry cases, and Playwright input diagnostics in `tests/audio-input.spec.ts` |
| `sru-12` (modified) | covered | `TestBackFromConfigurationPageSavesRuntimeConfiguration`, `TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot`, JUCE runtime-shell Sync save/reopen, browser Sync Back persistence |
| `sru-31` | covered | portable Sync surface assertions, `TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot`, `TestBrowserSyncUsesSharedStagingPersistsAndResolvesSourceNames`, JUCE runtime-page/session and fake-app Playwright Sync cases |
| `scw-1` | covered | `controller_wizard_tests` typed wizard/form ownership, validation refusal, and wrong-form mismatch cases plus `check-ui-boundary` |
| `scw-2` | covered | `controller_wizard_tests` registry-order, case-insensitive exact alias, fuzzy-rejection, unmatched-diagnostic, exclusivity, and Active/Blacklisted claim cases |
| `scw-3` | covered | `controller_wizard_tests` Twister form geometry/choice/enablement/numeric cases and generation cases; `instrument_tests` kind-valid generated slot |
| `scw-4` | covered | `controllers_page_ui_tests` submit/ignore/reconfigure/refusal cases, `MfTwisterSeedExtractionRequiresOneExactRepresentableProfileShape`, host save tests, and JUCE reconfigure/refusal simulations |
| `smi-1` (modified) | covered | `instrument_tests` Active/Blacklisted validity, opaque wizard identity, dormant-profile, and cross-disposition uniqueness cases |
| `smi-2` (modified) | covered | `instrument_tests` schema-2 round trips for both dispositions, previous-schema migration, and atomic rejection cases |
| `smi-3` (modified) | covered | `reconcile_tests` four blacklist-disposition cases and `mark_unconfigured_preserves_stored_refs_and_plan_order` |
| `smi-6` (modified) | covered | `blacklisted_slot_with_present_populated_refs_stays_unconfigured_and_inert` plus retained `startup_shaped_reconcile_one_of_two_controllers_present_no_failure` |
| `smi-8` (modified) | covered | `engine_tests` drop-only profile, Active/Blacklisted rebuild switch, and middle-slot resize cases; `TestActiveToBlacklistedTearsDownEndpointsAndDropsStaleBrowserCallback` |
| `smi-10` (modified) | covered | retained terminal-realtime cases plus the blacklisted drop-only and rebuild-switch `engine_tests` cases |
| `sru-4` (modified) | covered | `TestDiscoveryRendersPortableAvailableRowsAndDiagnostics`, `TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`, `ControllerLifecycleMutationsPreserveIdentityAndGateRegistryActions` (including both cross-disposition duplicate-rename directions), JUCE manual-record simulation, fake-app lifecycle Playwright |
| `sru-30` (modified) | covered | retained low-level relative-bank view-model/blocks cases plus the wizard-owned argument table in `controller_wizard_tests` |
| `sru-32` | covered | `controllers_page_ui_tests` session/chooser/submit/ignore cases, `TestThreeClickWizardSubmitCommitsThenSaves`, JUCE wizard parity simulation, fake-app three-click Playwright |
| `sru-33` | covered | `portable_ui_tests` wizard-session composition, `ControllersPageSimulationTests` parity simulations, fake-app Playwright, `check-ui-boundary` and `check:generic-runtime` |
| `sru-34` | covered | `TestDisabledSemanticNodesCarryEnabledState`, `ui-backend.spec.ts` disabled-control cases, `PortableJuceBackendTests` disabled semantic controls |
| `sru-43` | covered | `portable_ui_layout_tests` nesting/splice/insertion cases, and `controllers_page_ui_tests`' `SourceAssemblesUiNodeByHand` sweep over every producer source — which is the durable form of the requirement's grep-backed inspection scenario |
| `sru-44` | covered | `portable_ui_layout_tests` allocation, clamping, fraction, out-of-flow, overlay, wrapping, form-grid and metrics cases |
| `sru-45` | covered | `portable_ui_tests` per-kind colour/text-style cases, `PortableJuceBackendTests` carried-colour/derived-state cases, `ui-backend.spec.ts` carried-colour and derivation cases, `browser_command_buffer_tests` presence-flag round trips |
| `sru-46` | covered | `browser_command_buffer_tests` version-2 round trips and mismatch, `browser_runtime_contract_tests`, `PortableJuceBackendTests` fold/overhang/no-rescue cases, `ui-backend.spec.ts` parent-relative cases |
| `sru-47` | covered | `portable_ui_tests` Sync/Audio/File/Controllers geometry and appearance pins, `controllers_page_ui_tests`, `FilePageSimulationTests`, `ControllersPageSimulationTests`, `fake-app.e2e.spec.ts` |
| `sru-48` | covered | `visual-criteria.spec.ts` (eleven criteria plus anti-vacuity pins), `tests/support/VisualCriteria.hpp` consumed by `portable_ui_tests`, `portable_ui_layout_tests` mutation cases, and `ControllersPageSimulationTests` |
| `sru-49` | covered | `PortableJuceBackendTests` and `ui-backend.spec.ts` geometry property loops, `check-ui-boundary` deleted-policy-symbol scans |
| `sru-50` | covered | `TestExtentDrivenRedistribution`, `TestStandardLayoutRedistributesAtDifferentExtents`, `a second root extent redistributes weighted children in the rendered DOM` |
| `sru-51` | covered | `check-ui-boundary` (backend/codec include scans, deleted-symbol scans, standalone JUCE-free header compiles, and its own scanner self-test) plus `check:generic-runtime` |
| `sru-52` | covered | `PortableJuceBackendTests` and `ui-backend.spec.ts` click/drag/double-click/disabled/inert Draw cases with exact ordered action lists |
| `sru-53` | covered | `portable_ui_layout_tests` standard-layout cases, `braid4_system_tests`, `miniapp_system_tests`, `MiniAppJuceBackendParityTests` |
| `sru-54` | covered | `portable_ui_layout_tests` overflow-diagnostic and absorber cases, `portable_ui_tests` per-surface absorbing-region pins at the 480px floor and a taller surface |
| `sru-55` | covered | `portable_ui_tests` File-panel fill/border/radius pins, `browser_command_buffer_tests` border presence flags, `PortableJuceBackendTests` container-fill cases, `renders a container fill and rounded border across padding and gaps` |

## Requirement Mappings

### Browser app catalog launcher

| Requirement | Covered gates |
| --- | --- |
| `sbac-1` | `tests/catalog-client.test.mjs`: `loads configured catalogs concurrently and reports every healthy source`, `keeps healthy apps when sibling sources have network and version failures`, and `revalidates stable URLs on retry and discovers newly added manifest entries`; Playwright `tests/launcher.spec.ts`: `shows loading then accessible app metadata without requesting package files`. |
| `sbac-2` | Node `tests/catalog.test.mjs`: `parses a valid multi-app catalog and resolves package URLs against the catalog`, `rejects unknown schema-owned fields at every object level`, and `validates paths before URL resolution and rejects absolute or traversal-like paths`. |
| `sbac-3` | Node `tests/catalog.test.mjs`: `merges in configured source order, diagnoses duplicates, and sorts by display name then global ID` and `mergeCatalogs does not mutate source catalogs or depend on locale collation`. |
| `sbac-4` | Playwright `tests/package-loader.spec.ts`: `fetches and verifies every declared package file with CORS before creating object URLs` and `rejects stale or hash-mismatched WASM before importing the verified entry`; Node `tests/package-contract.test.mjs`: `changes the content-derived build ID when any emitted file changes`. |
| `sbac-5` | Native `projects/synth/tests/browser_runtime_contract_tests.cpp` and Playwright `tests/runtime-core.spec.ts`: `reads Emscripten browser contract versions without creating a runtime` and `rejects incompatible modules before creation or persistence setup`. |
| `sbac-6` | Playwright `tests/activation-lease.spec.ts` and `tests/launcher.spec.ts`: `reports package selection failure on its row and allows retry`. |
| `sbac-7` | Playwright `tests/package-loader.spec.ts`: `passes explicit immutable mappings to the Emscripten factory and refuses document-relative fallback`, `rewrites verified Emscripten import-meta worker bootstraps to typed object URLs`, and `revokes every materialized object URL exactly once when dispose is repeated`; `tests/two-origin-package.spec.ts`: `starts a generic verified package from the isolated launcher's second origin`. |
| `sbac-8` | Playwright `tests/launcher.spec.ts`: `locks selection after success and returns through generic top-level navigation`; `tests/static-site.spec.ts`: `launcher and runtime source remain application-generic`. |
| `sbac-9` | Playwright `tests/persistence.spec.ts`: `derives stable app-isolated patch roots from validated catalog identity` and `rejects incompatible runtime-config identity before filesystem or runtime initialization`; `tests/miniapp-smoke.spec.ts`: `real miniapp patch saves survive browser runtime restart through IDBFS`. |
| `sbac-10` | Node `tests/publish-site.test.mjs`: `publishes one deterministic first-party catalog deployment with complete package and rollback layouts`; Playwright `tests/static-site.spec.ts`: `published root discovers sheaf/miniapp without package bytes and selection uses only its immutable package`. |
| `sbac-11` | Node `tests/github-pages-workflow.test.mjs`: `workflow separates build, deploy, post-deploy validation, and cross-origin smoke responsibilities`, `deployed validator accepts every file in a complete CORS-readable catalog`, and `deployed validator rejects incorrect live WASM MIME delivery`; workflow `.github/workflows/synth-browser-pages.yml` runs `validate-deployed-catalog.mjs` and `tests/deployed-origin.spec.ts` after deployment. |
| `sbac-12` | `npm --prefix projects/synth/browser test`; `make -C projects/synth/browser browser-apps-smoke` (generic fixture gate followed by all configured first-party apps); Node `tests/catalog-client.test.mjs`: `revalidates stable URLs on retry and discovers newly added manifest entries`; Playwright `tests/two-origin-package.spec.ts` and `tests/activation-lease.spec.ts`. |
| `sprs-12` (modified) | `npm --prefix projects/synth/browser run publish:site`; Node `tests/publish-site.test.mjs`: `publishing identical inputs twice produces byte-identical complete trees`, `rejects missing and undeclared files without replacing the complete site`, and `site validation rejects changed bytes in either immutable package`; `.github/workflows/synth-browser-pages.yml` and `scripts/cloudflare-pages-build.sh` are the publish/build gates. |
| `sprs-8` (modified) | C++ `browser_runtime_contract_tests` preserves the shared `Runtime<App>` process contract; Playwright `tests/runtime-core.spec.ts`: `registers a supplied AudioContext module-locally and preserves direct handle zero` and `rejects modules missing context registration or native startup support`; `tests/audio-flow.spec.ts`: `fails closed when native AudioWorklet startup is unavailable`, `passes the already-resumed leased context to native startup without ring messages or another context`, and native-callback preference over JavaScript DSP; real-app `tests/first-party-apps-smoke.spec.ts`: `<app> launches once through the native catalog path with isolated persistence` observes one context, native callback progress, finite deadline data, non-silent output, stable memory after callback start, and no legacy fallback messages for Mini App and Braid 4. |
| `sbw-4` (modified) | `browser_runtime_contract_tests` and `browser_audio_device_tests` cover ABI-v4 input source registration, zero-input worklet options, clamped callback input shape, status publication, and System Default catalog behavior; Playwright `tests/audio-input.spec.ts` covers real-Wasm deterministic input, permission denial, shortfall, and stream end; `tests/first-party-apps-smoke.spec.ts` covers Mini App/Braid 4 zero-input no-prompt launches. |
| `sbw-10` | `tests/static-site.spec.ts` and publish-site validation cover microphone `Permissions-Policy` with cross-origin isolation; `tests/audio-flow.spec.ts` covers secure-context, permissions-policy, unavailable API, permission-denied, retry, unload, realtime-only paths, and native-worklet-only capture routing; `tests/audio-input.spec.ts` asserts the real-Wasm deterministic capture source connects only to the native worklet and never to `AudioContext.destination`. |
| `sbw-11` | `tests/audio-input.spec.ts` launches `AudioInputProbeApp` as real Wasm and observes native AudioWorklet peak for literal 1-, 2-, and 4-channel `ChannelMergerNode` source shapes, stronger four-live-channel published-count clamps, exact literal-zero silence, permission denial, unreported shortfall, stream termination, and teardown track-stop accounting; `tests/first-party-apps-smoke.spec.ts` observes zero-input Mini App and Braid 4 without `getUserMedia`, media-stream source construction, or input-source ABI registration. |
| `sbap-1` | Node `tests/app-build-manifest.test.mjs`: `validates, sorts, hashes, and deeply freezes the declarative manifest`, `generates the complete browser binding without app-specific plumbing`, exact-key/type/duplicate-ID cases, and source-root/header rejection cases; `tests/check-generic-runtime.mjs` rejects concrete application knowledge outside the manifest and permitted generated/test artifacts. |
| `sbap-2` | Node `tests/build-browser-apps.test.mjs`: `builds every record through one argument-vector compiler policy and writes structured emissions`, `does not replace the last complete emission report when a compile fails`, `can isolate fixture emissions and their report from the production output root`, and `rejects a successful compiler invocation that omits required artifacts`; the shared compiler vector asserts ABI-v4 exports, native audio registration/start boundary, and `536870912` initial, growable, `2147483648` maximum memory policy. |
| `sbap-3` | Node `tests/publish-site.test.mjs`: `assembles a deterministic complete multi-app catalog from the exact matching emission report`, `rejects a stale unrelated fixture report instead of packaging its output`, `rejects missing and undeclared files without replacing the complete site`, `publishes launcher assets, both packages, and one generic rollback page per app`, `publishing identical inputs twice produces byte-identical complete trees`, and `publishes only the complete two-app catalog tree to Pages and replaces it atomically`; `tests/github-pages-workflow.test.mjs`: `deployed validator checks the second app's package responses too` and `deployed validator rejects an unexpected whole catalog version`. |
| `sbap-4` | `tests/check-generic-runtime.mjs` and Playwright `tests/runtime-core.spec.ts`: `browser worker contains no concrete application branch`; fixture compiler coverage in `tests/build-browser-apps.test.mjs`; real Chromium `tests/first-party-apps-smoke.spec.ts` launches Mini App and Braid 4 through the same native catalog path with isolated persistence; `tests/deployed-origin.spec.ts`: `<app> launches once from the complete deployed catalog`; workflow and publication tests prove Cloudflare has launcher/runtime/rollback/header assets while Pages has only the complete publisher catalog/package tree. Local tests are loopback/artifact evidence only; `.github/workflows/synth-browser-pages.yml` owns live Pages CORS/MIME, whole-catalog-version validation, and deployed-origin smoke evidence after deployment. |

### `sprs-1` - Shared Portable Composition

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestSidebarOpensEachPageAndBackRestoresApp`,
  `TestAppActionsRouteOnlyToAppSurface`, and
  `TestRuntimeActionsRouteOnlyToOwningPageOrServices`.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp):
  `TestBrowserRuntimeUsesSharedFrameAndActionRouting`.
- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp): the
  `runtime_shell_session_tests` executable assertions that the JUCE shell owns
  exactly one portable renderer rooted at `runtime.main.root`, and that sidebar
  navigation replaces and restores the app through that renderer.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `real fake-app WASM renders and refreshes the shared runtime shell`.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `real miniapp WASM renders the complete shared shell and portable pages`.
- [`runtime-core.spec.ts`](../browser/tests/runtime-core.spec.ts):
  `browser worker contains no concrete application branch`, together with
  [`check-generic-runtime.mjs`](../browser/tests/check-generic-runtime.mjs).

### `sprs-2` (modified) - Additive Layout And Root Validation

Restated by this change: the shell hands each backend a fully resolved tree and
PLACES already-resolved subtree roots rather than translating every descendant.
There is no per-descendant offset loop and no auto-flow anywhere in the path.

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestPlacingASubtreeRootPlacesEveryDescendant` and
  `TestSubtreesArriveFullyResolved` pin the placement contract;
  `TestRejectsASurfaceTooShortForTheRuntimeSidebar` pins the composition
  precondition, because the shell never invokes the resolver on its own
  composition and so sru-54's gate cannot see it; and
  `TestRejectsRootSizeMismatch`, `TestRejectsDuplicateNodeIds`,
  `TestRejectsUnknownChild`, `TestRejectsCycle`,
  `TestRejectsAppRuntimeNamespace`, `TestRejectsDisconnectedGraph`, and
  `TestRejectsMultiplyParentedDiamondGraph` pin root validation.
- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp):
  `TestComposedSubtreeRootsFoldWithOneOffset` covers the composite app/sidebar
  surface, including that an unbounded app control is not wrapped by a backend
  cursor.
- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp): the
  additive shell-width and unclipped 900-pixel app-draw assertions.
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp): the sidebar
  subtree's resolved row geometry, its out-of-flow warning badge, and the pin
  that the badge consumes no stacking space.
- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `places nested sidebar descendants from parent-relative bounds` and
  `reports stable generic errors for malformed node trees`.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `real fake-app shared shell remains non-overlapping at narrow width`.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `real miniapp WASM renders the complete shared shell and portable pages` and
  `real miniapp shared shell scales as one non-overlapping narrow surface`.

### `sprs-3` - Compile-time Host Services

- [`browser_audio_device_tests.cpp`](../tests/browser_audio_device_tests.cpp):
  `TestBrowserExposesOnlySystemDefaultOutput`,
  `TestBrowserDefaultSelectionPersistsAsEmptyName`,
  `TestBrowserRejectsNamedOutputSelection`, and
  `TestBrowserServicesExposeNegotiatedDefaultAudio`.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp):
  `TestBrowserPrepareFeedsNegotiatedAudioPageAndRejectsOversizedBlocks`,
  `TestSharedBrowserNavigationReplacesAndRestoresEveryRuntimePage`,
  `TestControllersUseLatestBridgeSnapshotCommitEditsAndSaveOnBack`, and
  `TestFilePageDispatchesPatchLifecycleThroughBrowserRuntime`.
- [`browser_midi_bridge_tests.cpp`](../tests/browser_midi_bridge_tests.cpp):
  `TestReconcileBindsSlotsIndependentlyAndResyncsOutputs`,
  `TestIncomingAndOutgoingSysexStayOnSelectedControllerSlot`,
  `TestOfflineSlotDoesNotRemapAnotherSelectedSlot`,
  `TestNameFallbackUpdatesStoredReferencesThroughTheBridge`, and
  `TestLatestDeviceListMatchesSubmittedEndpoints`.
- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp),
  [`RuntimePagesJuceTests.cpp`](../juce/RuntimePagesJuceTests.cpp),
  [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp), and
  [`FilePageSimulationTests.cpp`](../juce/FilePageSimulationTests.cpp): retained
  JUCE services/page behavior.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `real fake-app WASM renders and refreshes the shared runtime shell` opens and
  returns from Audio, Controllers, and File using the same generic binding.

### `sprs-4` - Browser Pointer Parity

- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `captures pointer drags and dispatches accepted incremental two-axis deltas`,
  `compensates pointer movement for the current surface scale`,
  `keeps captured drags alive outside and clears them on cancel and lost capture`,
  `does not begin a drag when pointer capture fails`,
  `allows only one pointer to drive each drag element`, and
  `uses the current surface scale for each accepted drag increment`.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `real fake-app WASM dispatches incremental drag and double-click actions`.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `real miniapp WASM preserves gestures across fresh frames`.
- [`MiniAppJuceBackendParityTests.cpp`](../juce/MiniAppJuceBackendParityTests.cpp):
  the `miniapp_juce_backend_parity_tests` encoder drag and double-click
  assertions pin the desktop behavior being matched.

### `sprs-5` - Rounded Arc Drawing

- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `draws arcs with isolated round cap and join state` uses a near-zero arc and
  verifies save/round-cap/round-join/restore ordering and subsequent line state.
- [`EncoderComponentGeometryTests.cpp`](../juce/EncoderComponentGeometryTests.cpp):
  the `encoder_component_geometry_tests` assertions pin dot-like resting arc
  geometry and portable encoder arc commands.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `real miniapp shared shell scales as one non-overlapping narrow surface`
  verifies all seven encoder canvases retain nontrivial pixels, and the desktop
  and narrow smoke tests write the reviewed runtime-shell screenshots.

### `sprs-6` (modified) - One Resolved Browser Coordinate System

Restated by this change: the browser backend positions every child from its
wire bounds directly. The parent-origin subtraction, the auto-flow cursor, the
default-size table and both coordinate classifiers are gone, and
`check-ui-boundary` fails if any of them reappears.

- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `offsets a child by its wire bounds with no parent subtraction`,
  `keeps an overhanging child parent-relative`,
  `does not flow a node without resolved bounds`,
  `does not flow unbounded controls after explicit draw content`,
  `derives the host height from the resolved root extent, not from content`,
  `renders every representative node at the fold of its ancestor origins`,
  `keeps scroll descendants out of the outer surface extent`,
  `places nested sidebar descendants from parent-relative bounds`,
  `keeps the scale transform only on the current parentless root`,
  `clips a long toggle label inside its resolved extent`,
  `fits long status text inside its resolved extent without moving the next control`,
  and `fits a fixed portable surface into a narrow browser viewport`. Text
  fitting is the only appearance decision the backend still makes, and these
  three cases are what pin it to fitting inside an extent the library resolved.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `real fake-app WASM renders and refreshes the shared runtime shell` and
  `real fake-app shared shell remains non-overlapping at narrow width`.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `real miniapp WASM renders the complete shared shell and portable pages` and
  `real miniapp shared shell scales as one non-overlapping narrow surface`.

### `sprs-7` - Cross-backend Acceptance

- JUCE-free C++: the exact `runtime_main_component_tests`,
  `browser_audio_device_tests`, `browser_midi_bridge_tests`, and
  `browser_runtime_contract_tests` cases listed above.
- JUCE: `portable_juce_backend_tests`, `miniapp_juce_backend_parity_tests`,
  `runtime_shell_session_tests`, and the retained runtime-page executables
  listed under `sprs-3`.
- Static site: [`static-site.spec.ts`](../browser/tests/static-site.spec.ts):
  `canonical static server applies browser isolation and MIDI sysex headers`,
  `static shell loads generic portable UI styling`, and
  `normal generic browser flows make no dynamic HTTP or WebSocket requests`.
- Audio: [`audio-flow.spec.ts`](../browser/tests/audio-flow.spec.ts):
  `starts a worklet from user activation and copies finite non-silent samples`
  and `real miniapp WASM renders four finite non-silent audio blocks`.
- MIDI: [`midi-flow.spec.ts`](../browser/tests/midi-flow.spec.ts):
  `requests Web MIDI sysex permission and remains offline when it is denied`,
  `routes sysex between selected ports and their independent controller slots`,
  `polling recovers missed port changes without remapping another slot`,
  `drains outbound MIDI on a fast cadence without polling endpoint snapshots`,
  and
  `real miniapp WASM keeps two Web MIDI controller slots independent through reconnect`.
- UI and visuals: the exact fake-app and miniapp gesture, page, desktop, and
  narrow tests listed under `sprs-1`, `sprs-2`, `sprs-4`, `sprs-5`, and `sprs-6`.

### `sprs-9` (modified) - Hierarchical Generic JUCE Backend

Restated by this change: the JUCE backend paints `Draw` commands node-local in
component space and translates node bounds unconditionally. The draw-geometry
classifier, the node-bounds classifier, the auto-flow engine and the per-variant
colour table are gone, and `check-ui-boundary` fails if any of them reappears.

- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp), which
  is now 27 named cases behind a dispatching `main()` so per-case failure
  evidence is obtainable without scaffolding:
  `TestRenderedPositionFoldsAncestorOriginsAndScrollOffset` walks every node of
  a representative tree and requires its rendered position to equal its wire
  bounds folded over its ancestor origins plus the scroll offset;
  `TestOverhangingChildBoundsFoldWithoutReclassification` and
  `TestNodeWithoutResolvedBoundsIsNotRescued` pin the two cases a classifier
  used to rescue; `TestDrawCommandsPaintNodeLocal` covers the node-local draw
  contract including fractional node dimensions;
  `TestRetainedControlsFollowSemanticReparenting` and
  `TestRetainedControlsFollowThePortableTreeNotTheirOwnState` cover retained
  semantic hosts and stable editor reparenting; and
  `TestContainerNodesRenderAsPanelsAndPaintNoLabel` covers container hosting.
- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `paints node-local draw commands into positioned canvases`,
  `paints line endpoints in node-local coordinates`,
  `paints every command in a draw-node buffer with node-local geometry`,
  `paints a geometry-free command without displacing the rest of the buffer`,
  and `paints an overhanging draw buffer node-locally with no classifier
  fallback` pin the same node-level contract in the browser backend.
- [`MiniAppJuceBackendParityTests.cpp`](../juce/MiniAppJuceBackendParityTests.cpp)
  and `matches JUCE backend geometry and carried style assignments` pin that the
  two backends agree on geometry and on carried style assignment.

### `sprs-10` - Real Generic JUCE Scroll Areas

- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp): the
  `portable_juce_backend_tests` cases named by the exact assertions
  `scroll area owns a JUCE viewport`,
  `scroll rows are hosted by the viewed content component`,
  `scroll content uses declared two-axis extent`,
  `viewport scrolls on both axes`,
  `final button is reachable after scrolling`,
  `viewport position survives refresh for a stable scroll node`, and
  `viewport position is clamped when content shrinks` cover the viewport,
  content extent, reachability, retained position, and extent-shrink contract.

### `sprs-11` - Production Generic Controllers Coverage

- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp): the
  `runtime_shell_session_tests` Controllers cases named by the exact assertions
  `controllers back renders through the shared renderer`,
  `controller rows render through the shared renderer`,
  `controllers header controls end above the scroll viewport`,
  `controller rows resolve to distinct surface positions`,
  `expanded controllers content is taller than its viewport`,
  `final mapping control is reachable at the maximum vertical scroll position`,
  and `final mapping control remains enabled and editable after scrolling`.
- [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp):
  `ControllersPageSimulationTests passed seed=0x5eaf2026` retains the 250-step
  action/commit simulation while rendering the real `ControllersPageSurface`
  through `synth_juce::PortableComponent` and comparing containment in common
  surface coordinates.

### `sru-1` (modified) - Main Pane, Sidebar, And Content Host

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestCompositeBoundsPreserveAppAndAddSidebar` and
  `TestSidebarOpensEachPageAndBackRestoresApp`.
- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp): one
  JUCE renderer, synchronous Audio-page replacement/Back, retained app surface,
  additive content width, and unclipped full-width app draw.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp):
  `TestSharedBrowserNavigationReplacesAndRestoresEveryRuntimePage`.
- [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts) and
  [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts): the complete
  shell/page tests and their corresponding narrow non-overlap tests.

### `sar-10` (modified) - Runtime-hosted Application Component

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestRefreshUpdatesRuntimePageModelsAndRollingDeadline`, action-routing tests,
  and page navigation tests.
- [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp): the
  desktop runtime owns and refreshes one shared main component while retaining
  the app surface across page navigation.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp):
  `TestBrowserRuntimeUsesSharedFrameAndActionRouting` and the browser runtime-page
  service cases listed under `sprs-3`.
- [`runtime-core.spec.ts`](../browser/tests/runtime-core.spec.ts):
  `main bootstrap composes runtime, UI, audio channels, and actions generically`
  and `static auto boot uses the default worker runtime client and receives idle status`.
- [`miniapp-smoke.spec.ts`](../browser/tests/miniapp-smoke.spec.ts):
  `miniapp smoke wiring keeps the generic fake-app gate first` verifies the typed
  browser entry names only the application type; the real-WASM shell and gesture
  cases verify the resulting host behavior.

### `sdsp-34` - Shaped Interpolation And Correlated Increments

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `shaped_interpolate_endpoints_and_landmarks`,
  `shaped_interpolate_preserves_double_progress`,
  `correlated_increments_use_reciprocal_center_and_hz_sigma`,
  `correlated_increments_floor_near_zero_rate`, and
  `correlated_increments_reject_invalid_config` cover cosine-table interpolation,
  time-domain center sampling, rate-domain per-voice spread, precision, floors,
  and invalid inputs.

### `sdsp-35` - Deterministic Ganged Voice

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `ganged_random_lfo_voice_runs_wait_move_and_done_states` covers default done,
  waiting and moving increments, exact and overshot boundaries, output holds,
  double progress, and reset source/target semantics.

### `sdsp-36` - Ganged Random Processor And Coherent Snapshot

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `ganged_random_lfo_samples_round_in_canonical_logical_order`,
  `ganged_random_lfo_slowest_voice_gates_round_turnover`,
  `ganged_random_lfo_floors_heavy_tail_increments`,
  `ganged_random_lfo_fixed_seed_is_reproducible`, and
  `ganged_random_lfo_validates_setup_and_uses_fixed_storage` cover correlation,
  turnover, deterministic injection, validation, and realtime-safe fixed storage.
- The `ganged_random_lfo_snapshot_*` cases in the same executable cover explicit
  publication of every live field and voice color, no recorded history, odd or
  changing revisions, bounded retries, and unchanged destination on failed read.

### `spv-6` - Predictive Ganged-LFO Round Visualizer

- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp):
  `TestGangedRandomLfoVisualizer` covers the shared maximum-duration axis,
  per-voice waiting/movement/hold paths, reconstructed present dots, solid past,
  dashed future, snapshot colors, fixed geometry bounds, and invalid snapshots.
- [`MiniAppJuceBackendParityTests.cpp`](../juce/MiniAppJuceBackendParityTests.cpp):
  `miniapp_juce_backend_parity_tests` renders the predictive polylines and dots
  through the existing JUCE backend.
- [`browser_command_buffer_tests.cpp`](../tests/browser_command_buffer_tests.cpp):
  `TestPredictiveGangedLfoUsesExistingDrawSchema` proves the browser consumes the
  same commands without a protocol-version change or diagnostic.

### `ssm-1` - Opt-In Owned Standard Bundle

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `standard_modulators_owns_address_stable_source_and_visualizer_storage`
  checks the retained target group, all processors, stable output and pointer
  rows, and distinct visualizers for `<1>`, `<2>`, and `<4>` specializations.
  Adjacent compile-time assertions reject copy and move construction and
  assignment for every specialization.

### `ssm-2` - Editable MIN-16 Configuration

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `standard_modulators_defaults_match_min16_contract` checks exact indexes,
  names, short names, source colors, and voice palettes;
  `standard_modulators_pre_registration_overrides_are_registered` exercises
  every configurable family; and
  `standard_modulators_configuration_freezes_after_registration` proves the
  mutable accessor closes without changing registered addresses or metadata.

### `ssm-3` - Four Derived Random Time Scales

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `standard_modulators_defaults_match_min16_contract` checks waiting means
  `0.5/2/6/16`, target sigmas `0.1/0.3/0.2/0.1`, waiting external sigmas
  `0.3W`, moving external sigmas `0.15W`, waiting internal sigmas `0.2/W`,
  and moving internal sigmas `0.4/W` for each waiting mean `W`.

### `ssm-4` - Atomic Fifteen-Source Registration

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp): the exact
  `standard_modulators_rejects_each_out_of_range_active_index_atomically`,
  `standard_modulators_rejects_each_duplicate_active_index_atomically`,
  `standard_modulators_rejects_invalid_random_timing_atomically`,
  `standard_modulators_rejects_empty_active_metadata_atomically`,
  `standard_modulators_rejects_wrong_voice_palette_size_atomically`,
  `standard_modulators_rejects_mismatched_group_shape_atomically`, and
  `standard_modulators_rejects_double_registration_without_mutation` cases
  cover validation and no partial mutation.
- `standard_modulators_mono_omits_constant_and_ignores_constant_collision`
  covers complete mono omission and inactive constant-index collision handling.

### `ssm-5` - Explicit Standard-Modulator Lifecycle

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `standard_modulators_lifecycle_requires_registration_and_finite_preparation`,
  `standard_modulators_process_advances_dynamic_sources_once_and_copies_voice_order`,
  and `standard_modulators_group_updates_and_ui_publication_remain_explicit`
  cover preparation/re-preparation, one-step random/noise advancement, immutable
  constant storage, stable addresses, caller-owned group updates, and
  block-controlled random snapshot publication.

### `spm-71` - MiniApp Standard Modulation Topology

- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_standard_fifteen_source_topology_without_changing_performer_topology`
  checks fifteen modulators, capacity `192`, all sixteen physical positions,
  standard sources at `0..3/11/14`, application sources at `4/5/6`, disconnected
  gaps, all depth cells plus return, stable visualizers, and unchanged performer
  topology.
- `miniapp_processes_and_publishes_ganged_random_lfo_at_audio_block_boundaries`
  checks host-rate preparation, per-sample process-before-update ordering, voice
  order, and block-boundary publication;
  `miniapp_main_layout_draws_bounded_scope_stack_and_complete_encoder_grid`
  checks the stacked VCO/LFO scopes, all sixteen bounded encoder cells at both
  default and compact sizes, absence of a separate main-screen ganged-random
  panel, and retained modulation-depth underlays.
- `miniapp_loads_old_six_index_depth_data_without_alias_or_translation` proves
  old saved depth numbers load literally while the live fifteen-source metadata
  remains authoritative. `miniapp_rig_patch_save_perturb_load_round_trip`
  covers current parameter persistence.
- `TestMiniAppTwoScopeCommandsUseExistingBrowserSchema` in
  [`browser_command_buffer_tests.cpp`](../tests/browser_command_buffer_tests.cpp)
  covers unchanged-version, diagnostic-free VCO/LFO command serialization;
  [`PortableDrawGeometryTests.cpp`](../juce/PortableDrawGeometryTests.cpp)
  checks the exact four grid corners; and
  [`MiniAppJuceBackendParityTests.cpp`](../juce/MiniAppJuceBackendParityTests.cpp)
  covers two-scope paint output, unchanged rendered origins for both scopes and
  all sixteen hosted encoder nodes, and position `15` push routing through the
  production JUCE backend.

### `sdsp-33` - MiniApp Scope Visualizers At `4/5/6`

- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_distinct_scope_visualizers_for_modulators` checks distinct
  retained VCO/VCO/LFO scope instances at `4`, `5`, and `6`;
  `miniapp_registers_standard_fifteen_source_topology_without_changing_performer_topology`
  distinguishes those from bundle-owned standard visualizers.

### `sdsp-38` - MiniApp Standard Noise

- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_noise_at_standard_index_fourteen` and
  `miniapp_publishes_new_noise_values_before_each_modulation_update` check index
  `14`, two stable voice pointers, bundle ownership, and process-before-update.
- `TestStandardModulatorVisualizersRemainPortable` in
  [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp) and
  `TestStandardModulatorUnderlaysUseExistingBrowserSchema` in
  [`browser_command_buffer_tests.cpp`](../tests/browser_command_buffer_tests.cpp)
  cover portable and browser noise-underlay rendering.

### `sdsp-40` - MiniApp Standard Constant

- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_constant_at_standard_index_eleven_without_sample_work`
  checks yellow `Constant`/`Const` metadata at `11`, stable values `(0, 1)`,
  stable pointers, and unchanged values across processing blocks.
- `TestStandardModulatorVisualizersRemainPortable` and
  `TestStandardModulatorUnderlaysUseExistingBrowserSchema` cover retained
  constant-underlay drawing without a sample-path copy.

### `d4-1` - Three Fifteen-Modulator Braid4 Groups

- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `initializes_parameter_groups_banks_slot_and_scene_endpoints` checks the three
  heterogeneous groups and shared sixteen-position slot;
  `braid4_filter_storage_seeds_all_eighty_owned_caches_and_excludes_xy_and_nested_depths`
  additionally counts the 80 top-level Braid-owned filter states across those
  existing groups;
  `braid4_standard_bundles_register_exact_independent_sources` checks retained
  independent `<2>/<4>/<1>` bundles and addresses; and
  `braid4_groups_fit_sparse_fifteen_position_modulation_views` checks capacities
  and connected-only sparse materialization in each group.

### `d4-3` - Braid4 Internal-Sample Signal Ordering

- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `prepares_four_x_internal_rate_and_sequences_internal_subframes` checks all
  three bundles at four-times-host rate, exact process/update cadence, and one
  publication per block.
- `matrix_feedback_uses_current_vco_outputs_and_delays_only_modulator_consumption_one_internal_sample`
  checks current post-gain matrix inputs, the existing one-internal-sample
  application-source delay at index `4`, and current standard-source visibility.
- [`braid4_deadline_tests.cpp`](../tests/braid4_deadline_tests.cpp) checks release
  callback budgets at `44.1`, `48`, and `96` kHz.

### `d4-8` - Parallel Braid4 LFO Sources At `4/5`

- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `parallel_lfo_topology_banks_colors_and_modulator_slots` checks the parallel
  module, shifted frequency ranges, bank layout, and application slots `4/5`;
  `audio_and_lfo_outputs_publish_normalized_stereo_mono_and_quad_modulators`
  checks every normalized application source in all three groups.
- `matrix_feedback_uses_current_vco_outputs_and_delays_only_modulator_consumption_one_internal_sample`
  and `runs_finite_non_silent_stereo_audio_after_decimation` pin application
  source timing and audible-path isolation.
- [`module_tests.cpp`](../tests/module_tests.cpp):
  `braid_vco_supports_frequency_octave_shift_and_parameter_colors` keeps the
  LFO-only frequency shift separate from the shared Mod LPF Cutoff mapping.

### `d4-9` - Braid4 Standard Visualizers

- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `braid4_standard_modulation_view_renders_underlay_and_app_sources_remain_encoder_only`
  checks standard underlays and encoder-only application cells;
  `braid4_standard_bundles_register_exact_independent_sources` checks every
  visualizer pointer, mono index `11`, and cross-group non-aliasing.
- `TestBraid4StandardModulationViewsRemainPortable` in
  [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp) checks quad and mono
  node trees, including the disconnected mono constant cell.

### `spm-20` (modified) - 64-Bit Parameter UI Snapshots

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `parameter_ui_snapshot_owns_parameter_source_and_gesture_colors`,
  `ui_state_reports_affecting_masks_through_gesture_index_63`, and
  `randomized_message_bus_ui_state_simulation` cover atomic parameter snapshots,
  source/gesture colors, visible cells, signed and unipolar ranges, and 64-bit
  gesture-affecting masks through index 63.
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp): the exact
  `encoder snapshot preserves gesture bit 63` and
  `encoder renders gesture 63 as badge 64` assertions pass a real
  `Parameter::UIState` through `EncoderDrawStateFromParameter` and the portable
  renderer.

### `spm-25` (modified) - Message-Driven Randomized UI State

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `randomized_message_bus_ui_state_simulation` is the deterministic
  manager-owned gesture/UI oracle, including 64-bit masks, stable route source
  identities, inverse positions, current/target values, selected bank/view
  state, and reproducible seed/step/action/sample diagnostics.
- `message_bus_sparse_lifecycle_model_tracks_pins_collection_reuse_and_patch_load`
  covers message-driven bank open/close, reset, collection, compatible slot
  reuse under a distinct parent, and patch-load boundaries.

### `spm-72` - Sparse Top-Level And Active-Route Traversal

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `group_process_sample_visits_only_registered_roots` and
  `group_process_sample_phases_visit_only_registered_roots` verify the explicit
  phase traversals still visit roots only; and
  `recursive_local_compute_seeds_display_without_audio_rate_processing` cover
  the top-level `ProcessLite` boundary and recursive local-state refresh.
  `modulators_apply_active_uses_explicit_stable_source_indices`,
  `active_modulation_routes_preserve_identity_and_settling_tail`,
  `active_modulation_route_union_keeps_source_with_only_voice_one_nonzero`, and
  `active_modulation_routes_randomized_full_scan_oracle_and_work_bound` cover
  compact application, stable source identity, swap/removal, across-voice
  route union, settling tails, and sample-by-sample full-scan equivalence.
- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `braid4_parameter_processing_ignores_materialized_local_depths` and
  `braid4_sparse_work_counters_bound_inactive_capacity` compare equal internal
  subframe counts across baseline, all materializable neutral locals, sparse
  active routes, and 64 configured inactive gestures. Observer visit counts are
  the authoritative complexity contract.

### `spm-73` - Sparse 64-Bit Gestures

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `manager_gesture_count_supports_zero_through_64_and_rejects_65_without_mutation`,
  `gesture_masks_visit_only_active_bits_through_index_63`,
  `ui_state_reports_affecting_masks_through_gesture_index_63`, and
  `message_bus_and_patch_round_trip_gesture_indices_32_and_63` cover counts 0,
  1, 32, 33, and 64, rejected 65, sparse set-bit evaluation, UI masks,
  messaging, and persistence.
- [`instrument_tests.cpp`](../tests/instrument_tests.cpp):
  `MessageInJsonRoundTripsHighGestureIndex` and
  `ControllerGesture63SelectsAndEditsManagerGestureWhileBankMaskRemains32Bit`
  cover controller index 63 while preserving the separate 32-bit bank selector.
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp): exact badge assertions
  retain legacy labels through gesture 15 and distinguish gestures 16--63 with
  one-based labels 17--64.

### `spm-74` - Neutral Local-Node Reclamation

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `neutral_local_collection_reclaims_leaf_and_preserves_high_water_accounting`,
  `neutral_local_collection_retains_non_default_scene_state`,
  `neutral_local_collection_retains_inactive_latent_gesture_value`,
  `neutral_local_collection_retains_active_gesture_at_default_value`,
  `neutral_local_collection_retains_unsnapped_runtime_state`, and
  `neutral_local_collection_retains_nonzero_normalization_state` cover the
  complete neutral/default eligibility guards and high-water accounting.
- `neutral_local_collection_retains_parent_with_non_collectible_child`,
  `neutral_local_collection_collapses_recursive_subtree_bottom_up`,
  `neutral_local_collection_detaches_child_while_parent_route_finishes_settling`,
  and `modulation_view_pins_visible_locals_until_deselect_boundary` cover
  bottom-up ownership, detach ordering, settling, and live-view pinning.
- `neutral_local_reuse_stays_bounded_beyond_configured_capacity`,
  `randomized_neutral_local_collection_reuses_slots_without_stale_topology`,
  `patch_load_collection_preserves_high_gesture_nested_state_and_collects_default_omissions`,
  `eligible_collection_preserves_semantic_parameter_json`, and
  `message_bus_sparse_lifecycle_model_tracks_pins_collection_reuse_and_patch_load`
  cover bounded reuse, complete reset, persistence, semantic JSON, and
  lifecycle integration.

### `spm-75` - Disconnected Sources Are Empty Modulation-View Positions

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `modulation_view_leaves_disconnected_sources_empty_and_hides_explicit_depths`,
  `modulation_view_capacity_counts_only_connected_missing_depths`,
  `random_mod_maps_connected_ordinals_and_skips_disconnected_sources`, and
  `random_mod_with_no_connected_sources_is_a_noop` cover the generic Bank
  contract for sparse materialization, visible null cells, capacity, and
  connected-only randomization.
- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_standard_fifteen_source_topology_without_changing_performer_topology`
  proves nine connected depths, six disconnected physical gaps, and the
  unchanged return-cell position.
- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `braid4_standard_modulation_view_renders_underlay_and_app_sources_remain_encoder_only`
  proves the distinct polyphonic and monophonic sparse views, including the
  monophonic constant gap, while
  `braid4_groups_fit_sparse_fifteen_position_modulation_views` checks the
  connected-only materialization counts.
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp):
  `TestBraid4StandardModulationViewsRemainPortable` proves a disconnected
  modulation position has neither an encoder cell nor a visualizer node.

### `spm-31` (modified) - Relative And Absolute Encoder Input

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `midi_encoder_input_maps_scaled_turns_pushes_and_timestamps` and
  `midi_encoder_input_direction_only_zero_and_thru_behavior` retain both
  relative decoders. `midi_encoder_input_absolute_maps_raw_positions_independent_of_turn_step`
  checks raw CC 0, 64, and 127, exact timestamps/addresses, and two distinct
  stored turn steps. `midi_encoder_input_absolute_preserves_mapped_push_and_thru_boundaries`
  covers mapped zero consumption, push press/release behavior, and thru.
- `wrld_bldr_default_profile_maps_encoders_analogs_and_system_buttons`,
  `mf_twister_default_profile_maps_encoders_and_input_only_side_buttons`,
  `default_instrument_uses_shared_wrldbldr_default_profile`, and
  `miniapp_rig_default_instrument_has_single_wrldbldr_controller` retain the
  shipped relative defaults across library, Braid, and MiniApp boundaries.

### `spm-52` (modified) - Encoder And Absolute-Message Persistence

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `encoder_mode_contract_defaults_to_signed_7_bit`,
  `encoder_mode_json_round_trips_absolute_and_writes_new_field_only`,
  `encoder_mode_json_loads_legacy_field_and_migrates_on_save`, and
  `encoder_mode_json_new_field_is_authoritative` cover the renamed contract,
  all three modes, legacy fallback, new-field precedence, and migration.
- `param_set_absolute_message_constructs_and_round_trips_exact_payload` and
  `param_set_absolute_survives_controller_system_association_round_trip` cover
  the new message name/payload through generic message and profile JSON. The
  existing WRLD.Bldr, MF Twister, Launchpad, profile-factory, and invalid-load
  tests retain the complete profile compatibility boundary.

### `spm-76` - Exact Absolute Scene And Gesture Distribution

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `absolute_edit_locations_form_the_independently_computed_convex_system` and
  `absolute_edit_locations_cover_endpoints_no_gestures_and_aliased_storage`
  independently check the scene/gesture coefficients and alias aggregation.
  `absolute_projection_is_exact_minimum_change_and_redistributes_saturation`
  and `absolute_projection_handles_noop_endpoints_bipolar_ranges_and_rejects_invalid_contracts`
  cover the pure solver, true bipolar helper bounds, saturation, endpoints,
  minimum change, and invalid contracts.
- `handle_set_absolute_reaches_endpoint_mid_blend_and_aliased_scene_targets`,
  `handle_set_absolute_arms_then_rebuilds_the_proof_counterexample`,
  `handle_set_absolute_arms_both_touched_endpoints_and_preserves_unrelated_storage`,
  and `handle_set_absolute_clamps_input_keeps_normalized_bipolar_storage_and_rejects_nonfinite`
  cover focused production behavior, including normalized storage for bipolar
  presentation parameters and raw-center-before-slew checks.
- `handle_set_absolute_seeded_property_matches_independent_post_arming_model`
  uses seed `0xAB501`, 192 randomized cases, and forced endpoint, aliased,
  zero/one-weight, saturation, and post-arming topologies. Its oracle derives
  coefficients directly from the specification and solves the projection with
  independent bisection; it does not call either production math helper. Every
  case checks same-call arming, deterministic repeated output, all storage
  bounds, bitwise preservation of unrelated inactive storage, independent
  minimum-change results, and production raw-center error at most `1e-5`.

### `spm-77` - Absolute Message Routing

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `param_set_absolute_message_constructs_and_round_trips_exact_payload`,
  `param_set_absolute_survives_controller_system_association_round_trip`,
  `param_set_absolute_routes_by_selected_bank_slot_position_and_physical_encoder`,
  `param_set_absolute_edits_visible_modulation_depth_not_hidden_parent`,
  `param_set_absolute_is_blocked_by_every_effective_modifier`, and
  `param_set_absolute_unmapped_boundaries_are_no_ops` cover construction,
  serialization, owning-scene routing, selected-bank and physical addressing,
  visible modulation depths, every modifier, and all no-op lookups.

### `sru-26` - Controllers Absolute Encoder Mode Editing

- [`viewmodel_tests.cpp`](../tests/viewmodel_tests.cpp):
  `EncoderModeCatalogExposesAllChoicesInDeclarationOrder`,
  `AbsoluteEncoderModeHasItsOwnCatalogIndexAndRowValue`,
  `AbsoluteEncoderModeCommitKeepsOpenRowsAndRestoresStoredRelativeStep`,
  `EncoderModeIndexMustBeIntegralAndLeavesOutputUntouched`, and
  `EncoderModeIndexOutOfRangeIsRefused` cover the three-entry catalog, checked
  conversion, non-deletable stable session rows, retained relative-only step,
  persisted absolute config, live processor rebuild, and switching back.
- [`controllers_page_ui_tests.cpp`](../tests/controllers_page_ui_tests.cpp): the
  portable Controllers action path checks declaration-order labels, the
  relative-only step cue, structural non-deletability, commit/rebuild identity,
  live absolute decoding, and restored relative decoding in one open session.

### Sparse-Modulation Timing Evidence

- [`braid4_deadline_tests.cpp`](../tests/braid4_deadline_tests.cpp):
  `braid4_meets_48000hz_256_frame_deadline_and_continuity`,
  `braid4_meets_96000hz_256_frame_deadline_and_continuity`,
  `braid4_sparse_modulation_meets_48000hz_256_frame_deadline`, and
  `braid4_sparse_modulation_meets_96000hz_256_frame_deadline` print baseline and
  sparse-active average/p99 measurements at 48 kHz host/192 kHz internal and
  96 kHz host/384 kHz internal. These generous deadline ceilings are
  platform-sensitive smoke evidence; they do not assert a speedup ratio and do
  not replace the deterministic work-count contract above.

## Runtime Button-Grid Requirement Mappings

### `bgr-1` through `bgr-5` - Grid Core And Realtime Lifecycle

- [`button_grid_tests.cpp`](../tests/button_grid_tests.cpp):
  `grid_range_is_checked_signed_half_open_and_row_major`,
  `equal_range_slots_keep_selection_and_routing_independent`, and
  `grid_routing_calls_each_callback_and_ignores_invalid_targets` cover checked
  signed geometry, distinct equal-range slots, exact selection, callback order,
  duplicate rejection, and invalid/empty/disconnected no-ops.
- The same binary's
  `state_cell_modes_follow_toggle_momentary_set_only_and_show_only_contracts`,
  `state_cell_flash_policies_choose_palette_without_changing_on_off`, and
  `state_cell_observes_but_does_not_own_stack_state` cover every reusable cell
  and ownership scenario.
- `ui_publication_packs_on_off_and_clears_empty_disconnected_and_stale_cells`
  covers signed snapshot lookup, packed on/off bytes, and stale clearing.
  `finalization_rejects_late_topology_and_runtime_operations_keep_storage_stable`
  covers loud topology freeze and proves post-finalization reuse with unchanged
  vector capacities, backing-storage addresses, and `Grid*`/`GridSlot*`
  addresses. The framework therefore needs no parameter-style arena or runtime
  storage-growth message.
- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp)
  `message_bus_routes_parameter_and_grid_families_without_namespace_aliasing`
  covers the separate parameter and grid slot number spaces.

### `spm-75` through `spm-78` - Messages, Profiles, Pressure, And Feedback

- [`instrument_tests.cpp`](../tests/instrument_tests.cpp):
  `GridMessageInFactoriesCarryFlatSemanticFields`,
  `GridMessageInJsonUsesFlatPerVariantShapeAndRoundTrips`, and
  `GridMessageInJsonRejectsVelocityOutsideByteRangeWithoutMutation` cover all
  four grid variants, signed coordinates, byte values, selection indices, and
  JSON round trips. The mixed-family message-bus test above covers FIFO,
  namespace isolation, missing managers, and invalid targets.
- `BasicMidiPolyPressureRecognizesOnlyCompletePolyphonicAftertouch`,
  `PolyPressureProcessorStampsMappedPressureAndConsumesExactMatch`,
  `PolyPressureProcessorPassesUnmatchedAndNonPressureToThruExactlyOnce`,
  `PolyPressureProcessorRejectsInvalidConfigWithoutReplacingPriorConfig`, and
  `CreateMidiControllerProfileBuildsPressureOnlyAndSharedMixedThruChains` cover
  status `0xA0`, mapped pressure/timestamp stamping, all pass-through cases,
  atomic config validation, and the shared processor chain.
- `ControllerProfileJsonWritesSchemaTwoAndRoundTripsPressureInput`,
  `ControllerProfileJsonReadsVersionOneWithoutPressureAndPreservesLegacyData`,
  `ControllerProfileJsonRejectsInvalidPressureShapesAtomically`, and
  `InstrumentJsonKeepsEnvelopeSchemaAndDelegatesPressureProfileVersion`, plus
  [`blocks_tests.cpp`](../tests/blocks_tests.cpp)
  `NormalizeSortsPressureMappingsByLogicalTargetThenPhysicalAddress` and
  [`rig_tests.cpp`](../tests/rig_tests.cpp)
  `rig_runtime_config_round_trips_nested_pressure_profile_without_changing_envelopes`,
  cover backward reads, schema-2 writes, signed/canonical persistence, nested
  envelopes, and unchanged patch persistence.
- `GridFeedbackReadsOnlyTheImmutableRuntimeSnapshot` covers RGB/on-off lookup,
  negative and missing targets, and the no-live-tree boundary. Existing golden
  assertions in [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp)
  (`system_output_processors_debounce_reset_and_render_cc_and_wrld_bldr`,
  `launchpad_color_sysex_uses_controller_product_and_rgb_note`,
  `launchpad_output_processor_debounces_reset_and_uses_system_info`, and the
  WRLD.Bldr/Twister output cases) retain the prior MIDI bytes, caches, reset
  behavior, brightness/color budgets, and non-grid feedback.

### `sar-24` - Runtime Ownership Without Application Exposure

- [`engine_tests.cpp`](../tests/engine_tests.cpp)
  `engine_owns_stable_runtime_grid_state_and_routes_both_buses` covers one
  runtime-owned manager, initialization/finalization order, stable
  `RuntimeUIState`, both buses, pre-app pumping, publication, and shutdown
  lifetime.
- [`rig_tests.cpp`](../tests/rig_tests.cpp)
  `rig_existing_app_keeps_parameter_ui_contract_with_empty_grid_snapshot` and
  [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp)
  `miniapp_existing_surface_keeps_parameter_ui_contract_without_grid_integration`
  cover unchanged application initialization, parameter UI access, processing,
  and visible surfaces. Application grid creation, exposure, and rendering are
  intentionally out of scope, not deferred incomplete behavior.

### `sru-26` and `sru-27` - Controllers Derived Grid Presentation

- [`blocks_tests.cpp`](../tests/blocks_tests.cpp)
  `ExpandGridButtonProducesAtomicMomentarySystemAndPressurePair`,
  `ExpandGridBlockTraversesSignedExclusiveRangeXFastAndPairsEveryCell`,
  `ExpandGridBlockFailureLeavesBothOutputVectorsUntouched`, and the
  `ReconstructGridMappings*` cases cover JUCE-free exact pairing, physical to
  logical coordinate identity, signed exclusive rectangles, maximal blocks,
  atomic validation, canonical reconstruction, and orphan preservation.
- [`viewmodel_tests.cpp`](../tests/viewmodel_tests.cpp)
  `GridPresentationReconstructsWrldBldrBlockAndLaunchpadButtonWithoutPressureRows`,
  `GridOpenSessionOwnsPairsAndPreservesHiddenOrphanAcrossEditDeleteAndReopen`,
  `GridInvalidRectangleAndDuplicatePairEditsRefuseAtomically`, and
  `GridAddSkipsPhysicalAddressesOwnedByHiddenOrphans` cover stable edit
  sessions, add/edit/delete, momentary-only pairs, and lossless hidden data.
- `controllers_page_ui_tests` covers the portable Grid Button/Grid Block labels,
  signed fields, generic actions, stable node IDs, and absence of aftertouch,
  pressure, status, and standalone note controls. The generic JUCE portable
  backend and runtime-shell coverage render the same semantic Controls tree;
  `controllers_page_simulation_tests` runs the independent
  seed `0x6a1d2026` across 320 operations, including JSON reload, invalid
  atomic edits, row identity, hidden-orphan equality, and scroll reachability.

## Braid 4 Parameter-Cache Filtering Requirement Mappings

### `sdsp-6` - Reusable One-Pole State And Shared Alpha

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `one_pole_filters_and_tanh_follow_dsp_contract` compares the cutoff-bearing
  path with caller-supplied alpha, advances two independent states with the
  same alpha, and confirms reset seeds a deterministic output. This is the
  primitive used by the Braid cache-filter bundles.

### `spm-11`, `spm-66`, And `spm-72` - Phase Boundary, UI Cache, And Root Traversal

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `process_lite_phases_replace_cached_knob_before_ui_smoothing` proves a
  between-phase replacement is clamped and becomes the UI input;
  `process_lite_wrapper_matches_explicit_phases` retains the consecutive-wrapper
  behavior; `parameter_process_sample_phases_recompute_only_in_phase_one`
  pins configured-cadence recomputation to phase 1; and
  `group_process_sample_phases_visit_only_registered_roots` keeps phase work
  off materialized local modulation-depth nodes.
- `ui_display_center_and_spread_follow_cached_knob_order` supplies the focused
  display-center/spread ordering check, while the existing active-route oracle
  cases continue to cover sparse route traversal.

### `smod-9`, `smod-10`, And `smod-11` - Controls, Matrix Coordinates, And LFO Styling

- [`module_tests.cpp`](../tests/module_tests.cpp):
  `braid_vco_registers_three_group_shapes_two_scenes_fourteen_red_parameters_and_sparse_bank`
  checks all four oscillator-indexed Mod LPF Cutoff IDs, names, defaults, and
  positions `8..11`; `braid_vco_maps_all_parameter_ranges_to_natural_vco_inputs`
  checks direct Phase mapping without the retired depth multiplier; and
  `braid_vco_supports_frequency_octave_shift_and_parameter_colors` keeps the
  cutoff range independent of the LFO frequency shift.
- `bipolar_matrix_registers_row_major_identity_parameters_and_bank_cells` names
  the `[output][input]` matrix coordinate convention, which the Braid ownership
  assertions use when assigning each row to one oscillator cutoff.

### `d4-1`, `d4-2`, `d4-7`, `d4-8`, And `d4-10` - Braid Ownership And Timing

- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `braid4_filter_storage_seeds_all_eighty_owned_caches_and_excludes_xy_and_nested_depths`
  enumerates every audible/LFO quad voice, cutoff, frequency, and matrix cell,
  counts 80 owned states, and excludes X/Y plus nested depths. It also observes
  seeding from current caches rather than a zero-origin transient.
- `braid4_owned_caches_share_row_cutoff_alpha_but_keep_independent_filter_state`
  checks output-row matrix ownership over every row/column, one alpha per
  oscillator after phase-1 cutoff sampling, ten independent states per
  oscillator, and identical filtered-cache visibility to DSP mapping and UI
  smoothing. `prepares_four_x_internal_rate_and_sequences_internal_subframes`
  and `matrix_feedback_uses_current_vco_outputs_and_delays_only_modulator_consumption_one_internal_sample`
  retain the phase ordering and one-internal-sample feedback timing checks.
- `braid_and_matrix_banks_expose_required_encoder_cells`,
  `parallel_lfo_topology_banks_colors_and_modulator_slots`, and
  `patch_save_perturb_load_round_trips_representative_braid_and_matrix_values`
  cover the stable control layout, parallel LFO bank, and persistence semantics.
- [`braid4_deadline_tests.cpp`](../tests/braid4_deadline_tests.cpp):
  `braid4_meets_44100hz_256_frame_deadline_and_continuity`,
  `braid4_meets_48000hz_256_frame_deadline_and_continuity`, and
  `braid4_meets_96000hz_256_frame_deadline_and_continuity` exercise the active
  filter path at 44.1, 48, and 96 kHz host rates. The required callback budget
  measurements are recorded in Task 4's verification report.

### `sdsp-42` - Phasor-To-Tick Crossings

- [`dsp_tests.cpp`](../tests/dsp_tests.cpp):
  `phasor2tick_priming_and_same_cell_processing_are_silent`,
  `phasor2tick_first_valid_process_silently_primes`, and
  `phasor2tick_emits_exactly_when_the_floored_cell_changes` cover silent
  priming, quiet same-cell work, and one tick at a floor-cell boundary.
  `phasor2tick_detects_backward_time_and_multi_cell_jumps_once_per_call`,
  `phasor2tick_rejects_invalid_inputs_without_corrupting_its_cell`, and
  `phasor2tick_processing_performs_no_dynamic_allocation` cover the observable
  discontinuity, validation, dependency-free, and realtime contracts.

### `smc-1` Through `smc-5` - Ownership, Timeline, Tempo, Transport, And Gates

- [`engine_tests.cpp`](../tests/engine_tests.cpp):
  `engine_owns_one_stable_clock_prepares_before_app_and_publishes_exact_current_plan`
  proves stable ownership, negotiated-audio preparation, and the exact plan
  pointer exposed through `AudioBlock`. The null/unprepared and default-wired
  sender cases cover deterministic startup and application delegation.
- [`master_clock_tests.cpp`](../tests/master_clock_tests.cpp):
  `acceptance_trace_internal_timeline_orders_half_open_fractional_transport_epochs`
  records stopped lifetime time, half-open endpoint ownership, exact adjacent
  anchors, a block-boundary tempo slope, fractional query/crossing results,
  Start, distinct Continue wire intent with a new current-run epoch, Stop, and
  transport-before-tick-zero ordering. The two `clock_plan_queries_*` cases and
  delayed-plan-history case retain direct affine-plan assertions (`smc-2`).
- `master_clock_receive_authority_rejects_manual_tempo_and_restores_it` and the
  transactional tempo/underflow cases cover conversion and manual/external
  authority (`smc-3`).
  `master_clock_internal_transport_uses_boundary_epochs_and_current_run_time`
  and `master_clock_external_start_arms_and_first_clock_is_timestamped_zero`
  retain focused Start/Continue/Stop and armed-first-clock coverage (`smc-4`).
  The receive/send policy, transport-only input, and PPQN-reprime cases exercise
  all four independent gates and phase-safe policy changes (`smc-5`).
- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp):
  `clock_plan_is_queried_at_every_fractional_oversampled_subframe` proves a 4x
  application maps each internal subframe to fractional output-sample time.

### `smc-6` Through `smc-8` - Acquisition, Recovery, And Regeneration

- [`master_clock_tests.cpp`](../tests/master_clock_tests.cpp):
  `acceptance_trace_external_acquisition_jitter_dropout_takeover_and_regeneration`
  records one provisional owner, foreign-source rejection, exact event-time
  acquisition, timeout/takeover, external Start and Continue activation on the
  first accepted clock, Stop, exact 24-PPQN input, alternating jitter,
  duplicate/out-of-order rejection, one inferred three-pulse gap,
  dropout/free-run, and regenerated output phase.
- The trace directly asserts recovered tempo is within `0.1 BPM` of 120 after
  64 stable intervals. The focused median/`1/8` PLL, outlier/multiple inference,
  bounded phase correction, jitter/dropout, source arbitration, long-period
  timeout, delayed timestamp mapping, and output-only splice cases isolate each
  `smc-6`/`smc-7`/`smc-8` rule. Engine's deterministic cross-bus timestamp merge
  proves drain order cannot replace event order.
- [`midi_sender_tests.cpp`](../tests/midi_sender_tests.cpp):
  `external_transition_regeneration_crosses_sender_with_fixed_offset_and_no_clock_hole`
  and `master_clock_events_cross_the_concrete_sender_without_deadline_replacement`
  verify that regenerated transitions retain phase and original deadlines
  across the concrete output lane.

### `smc-9` - Mapper And Scheduled Output

- [`master_clock_tests.cpp`](../tests/master_clock_tests.cpp):
  `acceptance_trace_mapper_output_calculation_reports_normative_maxima` records
  five callback errors, independently calculates their median and `1/32` EWMA,
  checks exact ordinary endpoint continuity, positive and negative 500-ppm
  future slopes, discontinuity generation, analytical fractional crossings,
  and regenerated fixed offset. It directly asserts calculated deadline error
  `<= 1 us`, spacing error `<= 2 us` plus a separately calculated 500-ppm slew
  contribution, and fixed-offset error `<= 1 us`.
- [`midi_sender_tests.cpp`](../tests/midi_sender_tests.cpp):
  `acceptance_trace_concrete_sender_broadcast_reconnect_cutoff_and_fallback`
  records identical ordered deadlines at two timestamp-capable outputs, an
  offline output that cannot stall its peer, future-only reconnect, transport
  before clock at an equal deadline, generation cutoff behavior, and the
  observable immediate-only fallback lane. Focused host-lead, lateness,
  overflow, reconnect snapshot, and cutoff cases remain separate regressions.
- These are deterministic calculation, queueing, and host-submission
  guarantees. They do **not** assert sub-microsecond sender-thread wakeup,
  browser-main-thread execution, OS MIDI service, cable, or physical-device
  delivery. JUCE's 1-ms default sink lead and the browser's 25-ms advertised
  lead provide host scheduling horizon; actual delivery quality remains
  observable through fallback/late/drop diagnostics.

### `smc-10` And `smc-11` - Persistence And Diagnostics

- [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp):
  `runtime_config_v1_migrates_to_default_sync_and_v2_save_contains_exact_sync_fields`,
  `runtime_config_v2_rejects_every_missing_or_wrong_sync_field_atomically`, and
  the runtime-config round-trip/file cases cover v1 defaults, exact schema-v2
  sync ownership, atomic validation, and save behavior. The patch-load cases
  prove sync configuration stays outside patch state (`smc-10`).
- [`engine_tests.cpp`](../tests/engine_tests.cpp):
  `engine_clock_diagnostics_publication_never_tears_known_tuples` and
  `engine_publishes_sensible_clock_diagnostics_before_and_after_audio` prove
  non-blocking coherent publication (`smc-11`). The portable, JUCE, and browser
  Sync page cases below expose BPM, lock/source, latency, ignored input, late,
  and dropped-output values without overwriting staged edits.

### `smi-10` Through `smi-12` - Terminal Input And Output Sinks

- [`instrument_tests.cpp`](../tests/instrument_tests.cpp):
  `MessageInRealtimeFactoriesPreserveInternalDefaultsAndExternalIdentity`,
  `RealtimeMidiProcessorTranslatesExactSingleByteMessagesWithOriginalTimestampAndSlot`,
  and `EveryControllerProfileEndsInRealtimeMidiIncludingEmptyProfiles` cover
  exact Clock/Start/Continue/Stop identities and timestamps, rejection of other
  bytes, terminal routing after profile rebuild, and empty profiles (`smi-10`).
  [`rig_tests.cpp`](../tests/rig_tests.cpp)
  `rig_empty_controller_accepts_raw_external_realtime_and_long_run_stays_finite`
  covers the end-to-end empty-profile path.
- [`midi_sender_tests.cpp`](../tests/midi_sender_tests.cpp): the concrete sender
  acceptance trace and focused broadcast, per-sink feedback isolation,
  non-blocking overflow, equal-order, and cutoff cases cover `smi-11`.
  `host_timestamped_broadcast_uses_the_max_registered_sink_lead`,
  `immediate_only_sink_waits_for_due_and_reports_fallback`, and
  `missing_timestamp_provider_uses_steady_clock_epoch_for_scheduled_delivery`
  cover the common sink contract and explicit degradation in `smi-12`.
- [`midi-flow.spec.ts`](../browser/tests/midi-flow.spec.ts) normalizes Web MIDI
  input into the shared time-origin-relative epoch and converts stored due times
  to Web MIDI timestamps while reporting throttled lateness.
  [`PortableDrawGeometryTests.cpp`](../juce/PortableDrawGeometryTests.cpp)
  checks that the JUCE adapter advertises host scheduling, converts the runtime
  epoch to JUCE monotonic milliseconds, and retains the future deadline and
  realtime byte in a one-event timestamped submission. The JUCE application
  aggregates build and exercise the concrete adapter. Neither host suite claims
  a physical MIDI device delivery bound.

### `sar-3`, `sar-6`, `sar-11`, And `sar-18` - Runtime Integration

- [`engine_tests.cpp`](../tests/engine_tests.cpp): stable clock ownership and
  preparation cover `sar-3`; timestamp-ordered drain, one commit/query/enqueue
  pass, exact plan delegation, null-plan startup, and allocation-free steady
  state cover `sar-6`. [`rig_tests.cpp`](../tests/rig_tests.cpp) exposes the
  deterministic clock injection/query/scheduled-output surface.
- [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp):
  `miniapp_registers_clocked_adsr_tempo_topology_without_changing_performer_topology`,
  `miniapp_clock_plan_drives_exact_adsr_gate_boundaries_across_nondivisor_blocks`,
  current-frame voice publication, tempo-authority, and Rig parameter cases
  cover the clock-related `sar-11` scenarios without changing app ownership.
- Engine's load-before-rebuild, missing-default, snapshot/save, and patch-
  exclusion cases, plus the schema-v2 cases under `smc-10`, cover `sar-18`.
  JUCE runtime-shell and browser Sync tests exercise save/reopen through both
  concrete hosts.

### `sru-2`, `sru-12`, And `sru-31` - Sync Runtime UI

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestSidebarOpensEachPageAndBackRestoresApp` opens Sync through the shared tab;
  `TestRefreshUpdatesRuntimePageModelsAndRollingDeadline` checks the rolling
  recent-peak deadline readout (`sru-2`).
  `TestBackFromConfigurationPageSavesRuntimeConfiguration` and
  `TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot` distinguish
  Audio/Controllers/Sync save-on-Back from File Back and prove atomic staged
  Sync commit, rejection, persistence, diagnostics refresh, and reopen
  (`sru-12`, `sru-31`).
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp) checks all safe
  defaults, four toggles, strict PPQN `1..960` validation, the non-24 warning,
  all diagnostics labels, and narrow layout. [`RuntimePagesJuceTests.cpp`](../juce/RuntimePagesJuceTests.cpp)
  renders that same model as JUCE controls; [`RuntimeShellSessionTests.cpp`](../juce/RuntimeShellSessionTests.cpp)
  exercises Sync navigation, staged edits, Back/save, and reopen.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp)
  `TestBrowserSyncUsesSharedStagingPersistsAndResolvesSourceNames` exercises the
  same portable model and next-block Engine handoff. The fake-app Playwright
  case `real fake-app WASM stages, validates, saves, and reopens Sync` verifies
  the real browser/WASM surface.

## Controller Configuration Wizard Requirement Mappings

### `scw-1` - Typed Controller Wizard And Portable Config Form

- [`controller_wizard_tests.cpp`](../tests/controller_wizard_tests.cpp):
  `ConfigFormOwnsStateAndDispatchActionMutatesIt` proves a form's state changes
  only through `DispatchAction` and that the caller owns it through the abstract
  `ControllerConfigForm` contract;
  `TypedWizardGeneratesProfileFromItsConcreteForm` proves the checked typed
  generation path; `TypedWizardRejectsInvalidFormBeforeGeneration` proves
  validation refusal precedes generation; and
  `TypedWizardRejectsDifferentConcreteFormWithoutGeneration` proves a form
  created by another wizard returns a type-mismatch error and produces no
  controller. That case is compiled in an `-DNDEBUG` test target so the
  production error path, not the debug assertion, is exercised.
- `make -C projects/synth check-ui-boundary` keeps `ControllerWizard.hpp`,
  `ControllerWizard.cpp`, and the forms free of JUCE/DOM headers; the form
  performs no device enumeration, engine edit, or save because those callbacks
  live on `ControllersPageCallbacks`, not on the form.

### `scw-2` - Baked Pair Registry And Candidate Classification

- [`controller_wizard_tests.cpp`](../tests/controller_wizard_tests.cpp):
  `ControllerWizardRegistryExposesStableMfTwisterDescriptor` pins the descriptor
  id `com.sheaf.midi-fighter-twister`, display name, kind, and alias lists.
  `DiscoveryMatchesMidiFighterTwisterByCaseInsensitiveExactAlias` and
  `DiscoveryRejectsPrefixSuffixAndImplicitNumberVariants` pin exact
  case-insensitive alias matching and the explicit rejection of prefix, suffix,
  and implicit-number names. `DiscoveryReportsUnmatchedNamesAndHalfPairs` covers
  the unmatched-endpoint diagnostics and half pairs.
  `DiscoveryReturnsCandidatesInRegistryOrderAndUsesEndpointOnce` and
  `DiscoveryPairsDuplicateDevicesByEnumerationOrder` cover registry order,
  endpoint exclusivity, and deterministic duplicate pairing.
  `DiscoveryClaimsStoredEndpointsByExactIdBeforeNameFallback`,
  `DiscoveryDoesNotFallbackByNameWhenExactIdLostContention`,
  `DiscoveryTreatsHalfConfiguredStoredRefsAsEndpointClaims`, and
  `DiscoverySuppressesPairsClaimedByActiveAndBlacklistedRecords` cover claim
  semantics across both dispositions and half-configured records.
  `DiscoveryResultsAreStableAndInputsRemainUnchanged` covers repeated identical
  results and non-mutation of the device list and instrument.
- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestSidebarWarningReflectsControllersDiscoverySnapshot` proves the derived
  available-candidate warning state.

### `scw-3` - MF Twister: One Encoder Slot And Exactly Six Buttons

- [`controller_wizard_tests.cpp`](../tests/controller_wizard_tests.cpp):
  `MfTwisterConfigFormPlacesSixButtonsInTwoColumnsOfThree` pins the two-column
  3+3 geometry and the left CC 8-10 / right CC 11-13 column bounds.
  `MfTwisterConfigFormBuildsClosedSixButtonSurfaceAndRoutesPortableActions` pins
  one `controller-wizard.twister.encoder-slot` defaulting to `0`, exactly six
  `...button.{N}.message` / `...button.{N}.argument` pairs, the six defaults
  (Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start, Previous Bank),
  the closed sixteen-choice option set with no None/unassigned entry, the
  wizard-owned argument-enablement table, and edits through portable actions.
  `MfTwisterConfigFormValidatesExactSizeTIntegerTextAndIgnoresDisabledArguments`
  covers `0`, `std::numeric_limits<std::size_t>::max()`, and rejection of empty,
  negative, non-base-10, whitespace, and overflowing text, plus the rule that
  disabled argument text cannot affect validation.
  `UISystemMessageHelpersExposeCatalogLabelsAndPreserveBankSlotArguments`
  pins the shared catalog helpers the form reuses.
- `MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm` proves all sixteen
  encoder positions' turn, push, and output mappings target the selected slot,
  six side associations occupy channel 3 CCs 8-13 with feedback disabled, hold
  choices emit matching `false` releases, Bank Select carries `slotIx` plus its
  `bankIx`, and Next/Previous Bank carry the form slot while ignoring disabled
  argument text. `MfTwisterWizardRefusesInvalidFormsAtomically` proves an invalid
  form installs nothing and retains its entered state.
- [`instrument_tests.cpp`](../tests/instrument_tests.cpp):
  `MfTwisterWizardGeneratesAnActiveKindValidInstrumentSlot` proves the generated
  slot is Active and valid under the `twister` kind rules.
- The narrower wizard argument table (`TwisterArgumentEnabled`) is deliberately
  not `UISystemMessageHasArg`; the low-level editor's Next/Previous Bank `Arg`
  behavior is covered separately under `sru-30`.

### `scw-4` - Lifecycle: Submit, Ignore, And Reconfigure

- [`controllers_page_ui_tests.cpp`](../tests/controllers_page_ui_tests.cpp):
  `TestWizardSubmitCommitsCompleteProfileThenSaves` proves one instrument commit
  with both endpoint references, the descriptor wizard id, and the generated
  profile, followed by a save request.
  `TestWizardSubmitRefusalsRetainFormAndPersistence` covers disconnected
  candidates, contended endpoints, and invalid fields refusing without commit or
  save while retaining form state.
  `TestWizardSaveFailureDoesNotRollbackCommittedInstrument` pins the documented
  save-after-commit behavior.
  `TestWizardIgnoreCommitsOneInertBlacklistedRecord` proves Ignore commits one
  Blacklisted record with the opaque wizard id, both endpoint identities, and no
  profile, then saves.
  `TestReconfigureSeedsExactProfilesAndReplacesOnlyTheValidatedRecord` covers
  compatible seeding, defaults-plus-destructive-warning for incompatible shapes,
  dormant-profile seeding on blacklisted rows, and preservation of name,
  endpoints, wizard id, and ordered position.
  `TestReconfigureRefusesEveryChangedExistingRecordIdentity` covers stale index,
  name, endpoint, and disposition refusal. Deterministic ` 2`/` 3` suffix naming
  is asserted inside the submit and ignore cases.
- `MfTwisterSeedExtractionRequiresOneExactRepresentableProfileShape` in
  [`controller_wizard_tests.cpp`](../tests/controller_wizard_tests.cpp) tests
  each shape mismatch independently: analog config, extra mappings, missing or
  altered default turn/push/output mappings, non-CC-8-13 or inexpressible
  associations, and a slot that is not common across encoders and bank messages.
- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp)
  `TestThreeClickWizardSubmitCommitsThenSaves` and
  [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp)
  `TestWizardSubmitRefusesACandidateRemovedSinceTheLastFrame` and
  `TestControllersUseLatestBridgeSnapshotCommitEditsAndSaveOnBack` prove the
  same commit/save contract through both host service implementations.
- [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp)
  `RunIncompatibleReconfigureSimulation` and
  `RunControllerWizardRefusalSimulation` drive the destructive-replacement and
  refusal paths through rendered JUCE controls.
- Playwright [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `controller wizard uses deterministic names for duplicate submitted Twisters`,
  `controller wizard stale and invalid submit preserve entered values`,
  `controller wizard reconfigure seeds exact-shape profiles`, and
  `controller wizard reconfigure warns and replaces incompatible profiles`.

### `smi-1` And `smi-2` (modified) - Disposition Model And Instrument JSON

- [`instrument_tests.cpp`](../tests/instrument_tests.cpp) model cases:
  `ActiveSlotsAcceptManualAndOpaqueWizardIdentity` (optional wizard id, opaque
  unknown ids, registry-independent validity);
  `BlacklistedIgnoredSlotsRequireIdentityButNoDormantProfile`;
  `BlacklistedSlotsRejectMissingWizardOrEndpointRefs`;
  `BlacklistedSlotsRetainDormantProfileWithoutRuntimeActiveConfig`;
  `BlacklistedDormantProfilesAreValidatedForKind`; and
  `AddControllerRejectsDuplicateNameAcrossDispositions`. The retained
  per-kind section/address-variant cases still cover the `smi-1` kind rules.
- JSON cases: `InstrumentJsonWritesActiveDispositionAndOmitsManualWizardId`,
  `InstrumentJsonRoundTripsWizardActiveWithOpaqueUnknownId`,
  `InstrumentJsonRoundTripsBlacklistedIgnoredWithoutProfile`,
  `InstrumentJsonRoundTripsBlacklistedDormantProfile`,
  `InstrumentJsonRoundTripsMixedDispositionsInOrder`,
  `InstrumentJsonRoundTripsControllersInOrder` (retained multi-kind ordered round
  trip), `InstrumentJsonLoadsPreviousSchemaControllersAsActiveWithoutWizardId`,
  and `InstrumentJsonIgnoresPreviousSchemaDispositionAndWizardIdExtensions`.
- Atomic rejection: `InstrumentJsonRejectsUnknownKind`,
  `InstrumentJsonRejectsUnknownDispositionAtomically`,
  `InstrumentJsonRejectsMalformedWizardIdsAtomically`,
  `InstrumentJsonRejectsDuplicateNames`,
  `InstrumentJsonRejectsActiveWithoutProfileAtomically`,
  `InstrumentJsonRejectsIncompleteBlacklistedRecordsAtomically`,
  `InstrumentJsonRejectsKindInvalidDormantProfileAtomically`,
  `InstrumentJsonRejectsLaunchpadWithEncoderMappings`,
  `InstrumentJsonRejectsLaunchpadWithWrldBldrPosition`, and
  `InstrumentJsonRejectsBadSchema`.
- Legacy patch documents (`smi-2` scenario "Legacy single-profile document loads
  with the section ignored"): `patch_json_ignores_legacy_midi_and_audio_sections_even_when_invalid`
  in [`parameter_modulation_tests.cpp`](../tests/parameter_modulation_tests.cpp)
  builds a patch root carrying malformed legacy `midiInstrument` and
  `audioDevice` sections and proves `ValidatePatchJSON` still succeeds,
  `LoadPatchJSON` applies the patch's parameter values, and the caller's
  instrument configuration and audio device state are left untouched. The test
  exercises those two legacy section names rather than the older `midiProfile`
  key; `ValidatePatchJSON` inspects only `schema`, `schemaVersion`, `patchName`,
  and `parameterValues`, so `midiProfile` is tolerated by the same
  unrecognized-key path.

### `smi-3` (modified) - Disposition-Aware Reconciliation

- [`reconcile_tests.cpp`](../tests/reconcile_tests.cpp):
  `blacklisted_slot_is_inert_and_does_not_claim_devices_needed_by_active_slot`
  proves disposition is checked before Active claiming and that a Blacklisted
  slot's deliberately stale references never contend;
  `blacklisted_slot_with_absent_populated_refs_marks_unconfigured_not_offline`
  proves disposition handling precedes configured-reference absence handling and
  emits no `MarkOffline`;
  `blacklisted_slot_with_present_populated_refs_stays_unconfigured_and_inert`
  proves no claim, open, reference-update, or resync action; and
  `active_to_blacklisted_closes_online_endpoints_then_converges_unconfigured`
  proves the close-then-unconfigure ordering and that the next plan is empty.
  Every retained Active matching, contention, offline, resync, and convergence
  case still passes unchanged.
- [`reconcile_executor_tests.cpp`](../tests/reconcile_executor_tests.cpp):
  `mark_unconfigured_preserves_stored_refs_and_plan_order` proves the two new
  generic actions update connection state without clearing stored references and
  preserve plan order.

### `smi-6` (modified) - Startup Connects Active And Skips Blacklisted

- Startup uses the same planner, entered with `Unconfigured` connection state.
  [`reconcile_tests.cpp`](../tests/reconcile_tests.cpp)
  `blacklisted_slot_with_present_populated_refs_stays_unconfigured_and_inert`
  is exactly that startup state for a Blacklisted slot whose devices are present
  and proves neither endpoint is opened, while
  `startup_shaped_reconcile_one_of_two_controllers_present_no_failure` retains
  the Active present/absent startup contract and
  `identifier_match_closed_input_opens_input_only` /
  `identifier_match_closed_output_opens_output_and_resyncs` retain later
  self-healing attach.

### `smi-8` And `smi-10` (modified) - Blacklisted Processor Construction

- [`engine_tests.cpp`](../tests/engine_tests.cpp):
  `blacklisted_midi_controller_profile_is_drop_only_and_emits_nothing` proves the
  slot holds a `DropMidiInProcessor`, an empty `inputThru`, empty `outputs`, no
  thru, and no terminal realtime processor, and that CC, SysEx, `F8`, `FA`, `FB`,
  and `FC` all emit nothing on the bus.
  `engine_rebuild_switches_active_and_blacklisted_processors_without_reading_blacklisted_config`
  proves Active-to-Blacklisted teardown and Blacklisted-to-Active rebuild both
  install the right chain, and that the Active chain keeps its terminal realtime
  routing (`smi-10`).
  `engine_rebuild_preserves_ordered_slots_when_blacklisted_middle_slot_is_deleted`
  proves slot resize and stable controller-slot identity after a middle removal.
  The retained `EveryControllerProfileEndsInRealtimeMidiIncludingEmptyProfiles`
  and realtime translation cases still cover the Active `smi-10` scenarios.
- [`browser_midi_bridge_tests.cpp`](../tests/browser_midi_bridge_tests.cpp)
  `TestActiveToBlacklistedTearsDownEndpointsAndDropsStaleBrowserCallback` proves
  the endpoints close and a stale host callback reaching the slot during the
  rebuild window is dropped.

### `sru-2` (modified) - Cached Classification And The Controllers Warning

- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp):
  `TestSidebarWarningReflectsControllersDiscoverySnapshot` proves the marker
  appears for an unclaimed recognized cached pair while the application view,
  not the Controllers page, is open;
  `TestWizardDiscoveryCacheRecomputesOnlyForCachedSnapshotChanges` proves an
  unchanged device source neither updates the cache nor recomputes discovery,
  that a device-list change recomputes it exactly once, and that a successful
  instrument commit recomputes against the cached devices and clears the now
  claimed candidate; and `TestThreeClickWizardSubmitCommitsThenSaves` proves the
  marker is gone on the refresh after a successful commit. The
  claimed/blacklisted suppression itself is proven by
  `DiscoverySuppressesPairsClaimedByActiveAndBlacklistedRecords`.
- [`browser_runtime_contract_tests.cpp`](../tests/browser_runtime_contract_tests.cpp)
  `TestBrowserControllerDiscoveryCacheUsesSignalsAndSuccessfulCommits` pins the
  same contract on the browser services.
- [`RuntimePagesJuceTests.cpp`](../juce/RuntimePagesJuceTests.cpp) renders and
  clears `runtime.sidebar.controllers.warning` as a JUCE label without replacing
  the Controllers entry. Playwright
  `controller wizard ignores an available row and restores warning after
  blacklist removal` covers clearance and return in the real browser.

### `sru-4` (modified) - Controllers List, Blacklisted Rows, And Lifecycle Actions

- [`controllers_page_ui_tests.cpp`](../tests/controllers_page_ui_tests.cpp):
  `TestDiscoveryRendersPortableAvailableRowsAndDiagnostics` covers the separate
  Available controllers area, its Configure/Ignore actions, and the
  unmatched-endpoint diagnostics.
  `TestControllerLifecycleActionsUseTheNormalCommitAndSavePath` covers Rename,
  Delete, Blacklist, Remove-from-blacklist, registry-gated action visibility,
  unknown-opaque-id recovery, the `Blacklisted` badge with stored endpoint labels
  and no mapping/endpoint controls, and commit-then-save routing.
  `TestEndpointSelectorsPreferTheExactStoredIdentifier` covers the Active
  endpoint choice list including an absent stored reference.
- [`viewmodel_tests.cpp`](../tests/viewmodel_tests.cpp)
  `ControllerLifecycleMutationsPreserveIdentityAndGateRegistryActions` covers
  `RenameController`, `DeleteController`, `BlacklistController`, and
  `RemoveFromBlacklist` on scratch instrument state: rename refused for an
  empty, unchanged, or already-used name and applied to Active, Blacklisted, and
  unknown-id rows; mandatory `config` to `dormantConfig` retention with name,
  kind, wizard id, and both endpoints preserved; Blacklist refused for a manual
  record, an unknown id, and an incomplete endpoint pair; and Delete/Remove
  preserving list order. The same case asserts the "Rename rejects duplicates"
  scenario directly in **both** cross-disposition directions: renaming the Active
  record to the Blacklisted record's name and renaming the Blacklisted record to
  an Active record's name are each refused with an "already exists" reason, and
  both prior names are retained with the live instrument unmutated. The model
  rule those refusals delegate to is additionally covered by
  `AddControllerRejectsDuplicateNameAcrossDispositions` in
  [`instrument_tests.cpp`](../tests/instrument_tests.cpp).
  The retained `AddControllerDuplicateNameFails`,
  `AddControllerLaunchpadSeedsDefaultProfile`, and
  `AddControllerGenericSeedsEmptyConfig` cases keep the manual "+" add contract.
- [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp)
  `RunManualRecordSimulation` proves a manually added record keeps Rename and
  Delete and is offered no Reconfigure, Blacklist, or Configure.
- Playwright [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts):
  `controller wizard supports rename and delete on active records`,
  `controller wizard actions are absent on a manually added record`,
  `controller wizard retains dormant profile when an active record is
  blacklisted`, `controller wizard configures a blacklisted record through its
  wizard`, and `controller wizard ignores an available row and restores warning
  after blacklist removal`.

### `sru-30` (modified) - Relative Bank Editing Versus The Wizard Table

- The low-level editor is unchanged and retains its coverage:
  [`viewmodel_tests.cpp`](../tests/viewmodel_tests.cpp)
  `RelativeBankSystemMessagesExposeCatalogAndKeepSlotsAcrossEdits` covers the
  single `Arg` interpreted as `slotIx`, argument preservation across Next/
  Previous Bank conversion, and rebuild stability;
  [`blocks_tests.cpp`](../tests/blocks_tests.cpp)
  `RelativeBankMessagesSortBySlotAndReconstructAsIndividualCanonicalRows` keeps
  them individual press-only rows through reconstruction and commit; the
  randomized controller view-model simulation still exercises them.
- The added sentence deferring wizard behavior to `scw-3` is covered by
  `MfTwisterConfigFormBuildsClosedSixButtonSurfaceAndRoutesPortableActions`
  (Next/Previous Bank arguments disabled) and
  `MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm` (their `slotIx`
  comes from the form-wide Encoder Slot).

### `sru-32` - Three-Click Configuration Wizard Flow

- [`controllers_page_ui_tests.cpp`](../tests/controllers_page_ui_tests.cpp)
  `TestWizardSessionRoutesPortableChooserAndForm` covers the visible-but-disabled
  zero-candidate action with its explanation, the unique candidate opening its
  form directly, the multi-candidate chooser with controller and endpoint labels,
  one open session at a time, chooser entries disappearing with their candidates,
  the one Encoder Slot plus exactly six rows in two columns, dispatch into the
  form, Back/Cancel leaving the instrument untouched, `Ignore this controller` on
  new-candidate sessions, and its absence on existing-record sessions.
  `TestWizardSubmitRefusalsRetainFormAndPersistence` covers refusal retaining
  every entered choice with an inline status.
- [`runtime_main_component_tests.cpp`](../tests/runtime_main_component_tests.cpp)
  `TestThreeClickWizardSubmitCommitsThenSaves` proves the whole path from the
  runtime shell requires only the Controllers, Configuration Wizard, and Submit
  activations, performs exactly one commit followed by one save, installs one
  Active record with the descriptor wizard id, both endpoint identifiers,
  sixteen turn mappings and six system associations, and clears the sidebar
  warning. The per-mapping slot-0 targeting is asserted by the Playwright case
  below and by `MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm`.
- Playwright [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts)
  `controller wizard unique candidate configures a default Twister profile in
  exactly three clicks` performs literally three `.click()` calls on
  `runtime.sidebar.controllers`, `runtime.controllers.wizard.launch`, and
  `runtime.controllers.wizard.submit`, checks the form's six defaults and 3+3
  column geometry before Submit, and then checks the installed record's kind,
  both reconciled endpoints, all sixteen encoder mappings on slot `0`, exactly
  six side associations, the cleared warning, and survival of a real runtime
  restart;
  `controller wizard disables configuration when no candidate exists`,
  `controller wizard presents a chooser for duplicate available Twisters`, and
  `controller wizard ignores from a new-candidate form` cover the remaining
  scenarios.
- [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp)
  `RunControllerWizardParitySimulation` drives the same sequence through JUCE.

### `sru-33` - Portable Wizard Backend Parity

- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp) proves the portable
  Controllers page exposes the enabled `runtime.controllers.wizard.launch` action
  and that dispatching it composes the form's own
  `controller-wizard.twister.encoder-slot` node together with
  `runtime.controllers.wizard.submit` and `runtime.controllers.wizard.ignore`
  into one tree.
- [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp)
  `RunControllerWizardParitySimulation`, `RunManualRecordSimulation`,
  `RunIncompatibleReconfigureSimulation`, and
  `RunControllerWizardRefusalSimulation` drive the production portable actions
  through rendered JUCE controls and compare node ids, labels, option ids and
  labels, selected values, enabled states, two-column bounds, dispatched actions,
  and resulting portable state against the JUCE-free expectations.
- Playwright [`fake-app.e2e.spec.ts`](../browser/tests/fake-app.e2e.spec.ts)
  covers the same ids and actions in real Chromium against real WASM, with
  test-controlled Web MIDI ports from
  [`helpers/fake-midi.ts`](../browser/tests/helpers/fake-midi.ts); persistence is
  verified by reloading the runtime, not by mutating C++ state.
- `make -C projects/synth check-ui-boundary` and
  `npm --prefix projects/synth/browser run check:generic-runtime` are the gates
  that keep wizard, Twister, blacklist, generation, and validation policy out of
  both backends.

### `sru-34` - Portable Semantic Enabled State

- [`browser_command_buffer_tests.cpp`](../tests/browser_command_buffer_tests.cpp)
  `TestDisabledSemanticNodesCarryEnabledState` proves the enabled flag crosses
  the command buffer for semantic control nodes.
- Playwright [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `renders disabled native controls and keeps their portable values`,
  `suppresses actions from disabled native controls`,
  `suppresses double-click and drag actions from disabled semantic nodes`,
  `stops an in-flight drag when its node becomes disabled`, and
  `keeps dispatching once a previously disabled node becomes enabled`.
- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp) renders
  disabled Button, ComboBox, TextField, Toggle, Slider, and Draw nodes as
  disabled JUCE components, preserves their values and selected options, and
  dispatches no action while disabled.
- The wizard-specific instance — a Next Bank, Previous Bank, or Start button
  disabling its paired argument — is covered by
  `MfTwisterConfigFormBuildsClosedSixButtonSurfaceAndRoutesPortableActions` in
  the portable tree, `RunControllerWizardRefusalSimulation` in JUCE, and the
  Playwright three-click case in Chrome.

## Portable UI Component Library Requirement Mappings

Added by the `rebuild-portable-ui-component-library` change. `sru-43`–`sru-55`
are new; `sprs-2`, `sprs-6`, and `sprs-9` are restated above.

### The version-2 UI wire format

Version 2 is a **hard break**. Both ends check `kCommandBufferVersion` /
`COMMAND_BUFFER_VERSION` for strict equality; there is no version-1 fallback and
no negotiation. Four things changed from version 1:

1. **Node bounds are parent-relative.** A node's `Bounds` are in its parent's
   space, the single parentless root's bounds are surface coordinates, and a
   `ScrollArea`'s children are relative to the scroll-*content* origin.
2. **`Draw` geometry is node-local**, against the owning node's own
   `(0, 0, width, height)` box, clipped to the node's bounds.
3. **`Node::color` and `Node::textStyle` cross the wire** behind explicit
   presence bytes, with the per-kind meaning of `color` fixed by `sru-45`. The
   `sru-55` container border — `borderColor`, `borderWidth`, `cornerRadius` —
   crosses the same way. A missing field is missing, never a sentinel a producer
   could also have chosen.
4. **`variant` is gone**, and so is the model field behind it. The residual set
   is empty: every one of the nine strings it carried decided appearance, and
   `color`/`textStyle`/`selected` carry all of it directly.

Every artifact that advertises the UI protocol version moves together and now
reads `2`: `kCommandBufferVersion` in
[`BrowserCommandBuffer.hpp`](../include/synth/browser/BrowserCommandBuffer.hpp),
`COMMAND_BUFFER_VERSION` in [`protocol.ts`](../browser/src/protocol.ts),
`synth_browser_ui_protocol_version()` in
[`BrowserRuntimeAbi.cpp`](../browser/cpp/BrowserRuntimeAbi.cpp), each Wasm
package's exported copy of it, and the wording in
[`catalog-schema-v1.md`](../browser/docs/catalog-schema-v1.md).

Covered by: `browser_command_buffer_tests` version-2 round trips, presence-flag
cases, and the retired-token sweep; `TestVersionMismatchFailsLoudly` and
`a version-mismatched buffer fails loudly and renders no frame`;
`browser_runtime_contract_tests` protocol-version assertions; and the publisher's
per-package protocol-version assertion, which reads the value out of each built
`.wasm` rather than trusting build order.

### `sru-43` - Single Hierarchical Authoring Library

- [`portable_ui_layout_tests.cpp`](../tests/portable_ui_layout_tests.cpp):
  nesting depth, component composition through callables,
  `TestInsertingARowShiftsSiblingsByExtentPlusGap`,
  `TestComponentResolvesIdenticallyUnderDifferentParents`, and the splice cases.
- [`controllers_page_ui_tests.cpp`](../tests/controllers_page_ui_tests.cpp): the
  grep-backed inspection. `SourceAssemblesUiNodeByHand` is run over
  `ControllersPageUI.hpp`, `RuntimePages.hpp`, and `ControllerWizard.cpp`, with
  both directions of the predicate pinned and an anti-vacuity fixture
  (`RuntimeMainComponent.hpp`, the one file that legitimately hand-places
  already-resolved subtree roots) proving the scan still fires. **This is the
  requirement's inspection scenario**; `check-ui-boundary` does not scan for
  hand-rolled node assembly and is not what covers sru-43.

### `sru-44` - Declarative Build-time Layout With Library-owned Metrics

- [`portable_ui_layout_tests.cpp`](../tests/portable_ui_layout_tests.cpp):
  `TestWeightsDivideRemainingSpaceDeterministically`,
  `TestMaximumClampsAndRedistributesOnce`,
  `TestClampingRedistributionDoesNotRepeat`,
  `TestFractionIsOfContentExtentNotRemainingSpace`,
  `TestUnclampedFractionPinsContentExtentBasis`,
  `TestInfeasibleMinimaFailLoudlyInDeclarationOrder`,
  `TestExplicitlyPositionedChildrenAreOutOfFlow`,
  `TestOverlayChildTakesItsTargetsResolvedBounds`,
  `TestOverlayRejectsATargetThatIsNotInFlow`,
  `TestOverlayInsideAnOverlayContainerResolves`,
  `TestOverlayTracksATargetTheFormGridMoves`,
  `TestFormGridAlignsLabelAndControlColumns`,
  `TestWrappingRowFlowsOntoAdditionalLines`,
  `TestInFlowDrawFactoryReceivesItsResolvedExtent`, and
  `TestTextReservationIsDeterministicAndBackendFree`.

### `sru-45` - Direct Colour And Text Style On Components

- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp): the per-kind colour
  meaning table, `TestUnstyledNodesCarryNothing`, and
  `TestCaptionIsAnEmittedLabelNodeNotAField`.
- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp):
  `TestCarriedColourDecidesTheButtonFill`,
  `TestCarriedTextStyleDecidesGlyphColour`,
  `TestSelectedPresentationDerivesFromTheCarriedColour`, and
  `TestDisabledAndContainerPresentationDeriveFromTheCarriedColour`.
- [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts):
  `renders one carried colour on the surface each node kind assigns it`,
  `derives selected and disabled presentation from the carried colour`,
  `derives hover and pressed presentation from the carried colour`, and
  `reads a checked toggle as selected when deriving its carried accent`.

### `sru-46` - Hierarchical Parent-relative Coordinates

Covered by the version-2 wire section above plus the `sprs-6` and `sprs-9`
sections, which pin the two backends' halves of the same contract.

### `sru-47` - Configuration Pages Rebuilt On The Component Library

- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp):
  `TestSyncPageAlignsThroughTheFormGrid`, `TestSyncPageFitsWithinTheRuntimeRoot`,
  `TestAudioSelectorsAreCaptionedWhileADeviceIsSelected`,
  `TestHiddenInputSelectorLeavesNoOrphanedCaption`,
  `TestFilePagePinsItsResolvedGeometry`,
  `TestFileIdleRegionPinsItsResolvedGeometry`,
  `TestFilePageCarriesPageColoursAndTextStyles`,
  `TestFilePanelsCarryAppearanceWithoutUnderlays`, and
  `TestFilePageDelegatesItsListsToSplicedSubtrees`.
- [`controller_wizard_tests.cpp`](../tests/controller_wizard_tests.cpp):
  `MfTwisterConfigFormResolvesItsExtentsFromItsDeclarationsAlone` pins the wizard
  form's extents now that they come from its declarations rather than from a
  producer-side table.
- [`FilePageSimulationTests.cpp`](../juce/FilePageSimulationTests.cpp) and
  [`ControllersPageSimulationTests.cpp`](../juce/ControllersPageSimulationTests.cpp)
  keep the model-based simulations green over the rebuilt pages.

### `sru-48` - Named Visual Criteria With A Playwright Verification Loop

- [`visual-criteria.spec.ts`](../browser/tests/visual-criteria.spec.ts) drives the
  real runtime shell over the fixture app's Wasm and evaluates all seven named
  criteria, each with an anti-vacuity count.
- [`VisualCriteria.hpp`](../tests/support/VisualCriteria.hpp) is the headless
  half, consumed by `portable_ui_tests` (every surface at three extents),
  `portable_ui_layout_tests` (one mutation case per criterion, plus a conforming
  twin), and `ControllersPageSimulationTests` (250 random Controllers states).
- `TestTheNamedCriteriaAreTheOnesThePlaywrightSuiteNames` parses the Playwright
  file's exported checklist and requires it to match the C++ list entry for
  entry, so the two halves cannot drift.
- Screenshot baselines were dropped from this requirement: appearance is agreed
  once with a human and is not pinned as a regression test. The machine-checkable
  criteria are the durable surface and they hold at any extent.

### `sru-49` - Backends Are Dumb Renderers

- The geometry property loops:
  `TestRenderedPositionFoldsAncestorOriginsAndScrollOffset` in JUCE and
  `renders every representative node at the fold of its ancestor origins` in the
  browser, both including the no-bounds-not-rescued case.
- `check-ui-boundary` fails if the auto-flow cursor, the default-size table,
  either coordinate classifier family, or the per-variant colour table reappears
  in a backend, and self-tests each of those patterns against a sample it must
  catch and a commented twin it must not.

### `sru-50` - Extent-driven Layout

- `TestExtentDrivenRedistribution` and
  `TestStandardLayoutRedistributesAtDifferentExtents` re-resolve the same
  producer code at two root extents.
- `a second root extent redistributes weighted children in the rendered DOM`
  does the same through the real backend, using a second fixture app that
  declares `uiHeight = 720`, because resizing the viewport re-resolves nothing.
- `Builder::Build` takes the root extent as an argument with no overload that
  reads it back off the tree, so no resolution path depends on a compiled-in
  surface size.

### `sru-51` - Enforced Layering

- [`check_ui_boundary.sh`](../scripts/check_ui_boundary.sh), run by
  `make -C projects/synth check-ui-boundary` and as a prerequisite of
  `make -C projects/synth test`.
- `check:generic-runtime` keeps first-party app knowledge out of the generic
  browser runtime.

### `sru-52` - Draw Node Click Actions

- [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp):
  `TestDrawClickOnlyDispatchesOnce`, `TestClickSequenceMatchesButtonExactly`,
  `TestDragDispatchesNoClick`,
  `TestClickAfterADragOnTheSameNodeStillDispatches`,
  `TestDisabledDrawDispatchesNothing`, `TestInertDrawInterceptsNothing`,
  `TestReleaseOutsideTheNodeIsNoClick`, and
  `TestDoubleClickSequenceMatchesButtonExactly`.
- The browser twins of the same eight cases in
  [`ui-backend.spec.ts`](../browser/tests/ui-backend.spec.ts), including real
  mouse double-clicks. Both suites pin the gesture sequence as an exact ordered
  action list with per-action counts for a `Draw` node and a `Button` node
  carrying the same actions.

### `sru-53` - Standard Synth Application Layout

- [`portable_ui_layout_tests.cpp`](../tests/portable_ui_layout_tests.cpp):
  `TestSlotsAcceptArbitraryComponents`,
  `TestStandardLayoutProportionsMatchBothApps`, `TestEmptyWidgetBayCollapses`,
  and `TestStandardLayoutRedistributesAtDifferentExtents`.
- [`braid4_system_tests.cpp`](../tests/braid4_system_tests.cpp) and
  [`miniapp_system_tests.cpp`](../tests/miniapp_system_tests.cpp): both apps
  compose the standard layout, every control resolves inside a declared region,
  and every scope stays individually bounded.

### `sru-54` - Every Container Absorbs Its Overflow Or Fails Loudly

- [`portable_ui_layout_tests.cpp`](../tests/portable_ui_layout_tests.cpp):
  `TestUnabsorbedOverflowFailsWithAnActionableDiagnostic`,
  `TestAnOverflowingRowNamesItsOwnStackingAxis`,
  `TestAScrollAreaAbsorbsAListTallerThanItsViewport`,
  `TestAWeightedChildAbsorbsTheRemainder`, and
  `TestAWrappingRowStillFailsOnAChildWiderThanTheRow`.
- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp):
  `TestEveryPageAndAppResolvesAtTheSmallestDeclaredSurface`,
  `TestEveryRebuiltPageAbsorbsAtTheSmallestDeclaredSurface`,
  `TestControllersChooserAndBraid4PinTheirAbsorbingRegions`, and
  `TestTheWizardFormIsReachableRatherThanClipped`. Each pins what the absorbing
  region actually does at the 480px floor and at a taller surface, not merely
  that resolution succeeded.

### `sru-55` - Container Background And Border

- [`portable_ui_tests.cpp`](../tests/portable_ui_tests.cpp):
  `TestFilePanelsCarryAppearanceWithoutUnderlays` — the File page's panels carry
  fill, border colour, border width, and corner radius directly, and the
  `PanelUnderlayFill` workaround is gone.
- [`browser_command_buffer_tests.cpp`](../tests/browser_command_buffer_tests.cpp):
  the three border fields round-trip behind their own presence bytes.
- `renders a container fill and rounded border across padding and gaps` in the
  browser and the container-fill cases in
  [`PortableJuceBackendTests.cpp`](../juce/PortableJuceBackendTests.cpp), with
  JUCE clamping the radius to half the shorter side and stroking an inset path
  so the declared outer radius matches the browser's inset shadow.


## Known Gaps

- Browser realtime audio underrun safety is intentionally not claimed by these
  mappings. The current deterministic scheduler deficit and the deferred
  render-ahead design are recorded in
  [`browser-audio-underrun-diagnosis.md`](browser-audio-underrun-diagnosis.md).
- Browser MIDI is bidirectional and covers SysEx, multiple selected devices,
  polling, disconnect, reconnect, and a low-latency output drain cadence.
  Overflow signaling for bursts beyond the bridge's bounded output queue should
  be handled as a follow-up once the browser shell lands.
