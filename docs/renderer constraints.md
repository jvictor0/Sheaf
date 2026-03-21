# Renderer Constraints

Client markdown/math rendering remains client-owned.

Server responsibilities:

- preserve message text fidelity
- stream token chunks in-order
- emit committed turns for reconnect replay

No server-side markdown transformations are applied.
