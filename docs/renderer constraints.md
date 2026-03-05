# Renderer Constraints

## Scope
This document defines the required behavior for Sheaf iOS chat markdown/math rendering.

## MUST: Message Chrome
1. User messages MUST render in a bubble.
2. Assistant messages MUST render without bubbles (main body style).
3. System messages MUST render without bubbles.
4. Tool event messages MUST render without bubbles.

## MUST: Markdown Subset
1. Supported block elements:
   - Headings (`#` to `######`)
   - Paragraphs
   - Unordered lists (`-`, `*`, `+`)
   - Ordered lists (`1.`, `1)`)
   - Block quotes (`>`)
   - Tables (header row + separator row + data rows)
   - Fenced code blocks (triple backticks)
   - Thematic breaks (`---`, `***`, `___`)
2. Supported inline rendering:
   - Standard inline markdown styling inside text nodes (bold, emphasis, inline code, links) where parseable.
   - Explicit line breaks from newlines inside paragraph content.

## MUST: Math Delimiters
1. Inline math MUST support only `\(...\)`.
2. Inline dollar math (`$...$`) MUST NOT be parsed as math.
3. Display math MUST support `\[...\]`.
4. Fenced math language blocks (`math`, `latex`, `tex`, `katex`) MUST render as display math unless they contain full-document LaTeX markers.
5. Full LaTeX document snippets in fenced blocks MUST render as code blocks.

## MUST: Width and Overflow
1. Renderer MUST avoid horizontal overflow off-screen for all message roles.
2. User bubble content and bubbleless assistant/system/tool-event content MUST have explicit max readable widths.
3. Inline layout MUST use parent-provided width constraints and MUST NOT depend on screen-width fallback heuristics.
4. Long inline math atoms that exceed line width MUST remain within the container (e.g., line-wrapped placement and/or local horizontal scrolling), never pushing the container off-screen.
5. Code blocks MAY use horizontal scrolling within their own block.

## MUST: Rendering Architecture
1. Markdown parsing MUST use CommonMark AST (`apple/swift-markdown`) as source of truth for block/inlines semantics.
2. Rendering MUST use a single structured document model (`RenderDocument`) as the UI contract.
3. Blocks and inline nodes MUST be rendered from parsed model output, not by reparsing rendered fragments in view code.
4. Math cache keys MUST remain stable for equivalent TeX input and block/inline mode.

## SHOULD: Accessibility and Interaction
1. Rendered content SHOULD allow text selection.
2. Tool-event visual style SHOULD remain secondary to assistant/user narrative.

## Regression Examples
1. Input:
   - `Its wonky on my end, if i do \(f(x)\) and then \(g(x)\) theyre on separate lines.`
   Expected:
   - `f(x)` and `g(x)` render inline in paragraph flow.
   - Content remains fully within screen bounds.
2. Input:
   - `Price is $5 and math is \(x+1\).`
   Expected:
   - `$5` remains literal text.
   - `\(x+1\)` renders as inline math.
