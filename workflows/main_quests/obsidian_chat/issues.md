# Issues

## Legacy iOS REST metadata endpoints drift from the live server

The iOS client model layer still defines `GET /threads/{id}/metadata` and
`GET /threads/{id}/messages`, but the current Python server does not expose
those routes. The live chat history contract is websocket handshake replay plus
`committed_turn` frames.

Status: `open`

Next Action: `Keep the Obsidian quest aligned to the live websocket contract and decide later whether the server should add the legacy REST endpoints back or the iOS client should retire them.`

## Tool call summary needs a stable filename extraction rule

The user-facing requirement is to show only the filename for read, write, and
patch tool calls. Current tool payloads may include keys such as
`relative_path`, `relative_dir`, or other tool-specific arguments. The planning
docs need a clear filename-only rule so the Obsidian pane does not accidentally
surface full paths or file contents.

Status: `open`

Next Action: `During implementation, add a shared summary helper for file-oriented tool calls and prefer basename extraction from known path fields with safe generic fallbacks.`

## Obsidian mobile-safe pane behavior still needs implementation-time validation

The existing plugin is mobile-compatible, but it does not yet host a custom
chat view. The quest should preserve mobile-safe behavior, yet workspace and
navigation details for desktop vs mobile will need hands-on validation in the
plugin runtime.

Status: `open`

Next Action: `Keep the spec mobile-conscious and treat desktop/mobile layout differences as a validation item before moving the quest out of polishing.`
