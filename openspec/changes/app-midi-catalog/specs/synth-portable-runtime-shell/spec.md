# Delta — `synth-portable-runtime-shell`

## ADDED Requirements

### Requirement: sprs-18 — Browser layout: a semantic control's own element fills its node box
WHEN the browser backend renders a combo box or a text field, THE backend SHALL size that node's `<select>` or `<input>` element to its node's own box — width and height 100%, border-box — and SHALL clip a `<select>`'s displayed text to that box, so neither element's intrinsic sizing from its option text or content ever grows past the bounds the component library resolved for its node.

#### Scenario: A combo box's select fills its node's box
- **WHEN** a combo box's `<select>` carries an option label longer than the node's resolved width
- **THEN** the `<select>` element's rendered width equals its node's wrapper width, not the option text's intrinsic width
- **AND** the displayed text is clipped to that width rather than overflowing past the node
- Check: `browser/tests/ui-backend.spec.ts`, "renders portable controls, canvas draws, and reachable scroll content" (select-fills-wrapper assertion)
