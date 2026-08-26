# Project instructions

These instructions apply to the complete repository.

## Project direction

[docs/project.md](docs/project.md) defines the project purpose and priorities.
Use its priority order when design goals conflict.

[docs/node-layout.md](docs/node-layout.md) contains the current node object
layout proposal. Do not treat a proposed choice as an approved design.

[docs/node-api.md](docs/node-api.md) defines node inputs, evaluation functions,
and output modification.

[docs/evaluation.md](docs/evaluation.md) defines evaluation order, context
services, lifecycle, scheduling, cycle callbacks, and failure behavior.

[docs/type-system.md](docs/type-system.md) defines type resolution, time series
guarantees, compatibility, propagation, and runtime enforcement.

[docs/time-series-references.md](docs/time-series-references.md) defines reference
routing, dynamic subscriptions, nested graphs, and removal lifetime.

[docs/time-series-validity.md](docs/time-series-validity.md) defines invalidation,
revalidation, and their effects on references, collections, and nodes.

[docs/time-series-types.md](docs/time-series-types.md) defines the initial time
series types and their change information.

[docs/tsd-layout.md](docs/tsd-layout.md) defines dictionary storage, shared
representations, and tabular buffers.

## Language

Use clear English and precise terminology.

- Use one term for one concept. Do not use synonyms only for variety.
- Use short, direct sentences. Prefer active voice.
- Avoid idioms, metaphors, and vague modifiers.
- State requirements as explicit actions or conditions.
- Expand an abbreviation at its first use unless it is a language or tool name.
- Use a technical term only when it is more precise than plain language.
- Do not restate the user's input unless you need clarification or identify a
  problem in the reasoning.

Follow [docs/terminology.md](docs/terminology.md). Do not add, change, or remove a
dictionary entry without explicit approval from the project owner. Propose a new
term before you use it as project terminology.

## Documentation

Keep documentation brief and complete.

- Include the information that a reader needs to act correctly.
- Put the purpose before details.
- Prefer short paragraphs and short lists.
- Give an example when prose alone can be misread.
- Do not repeat information that has one authoritative source.
- State each fact once. Remove text that only repeats or negates an implication
  of the surrounding text.
- Distill discussion into design statements. Do not preserve conversational
  wording or emphasis.
- Do not explain what is already clear from the code.
- Document the selected design. Do not record rejected alternatives.
- When a design choice follows from another requirement, state the dependency
  in positive language and link to its authoritative documentation. Use: “This
  is required to …”

## Code

Prefer the smallest clear implementation, the code should be elegant.

- Use existing utilities and the standard library before adding project code or
  a dependency.
- Do not add a helper for a single simple expression.
- Do not add a wrapper that only forwards or renames an interface.
- Add defensive checks only when a value can be null or invalid by design.
- Do not add ways to replace behavior, special object-creation code, or settings
  for an unconfirmed need.
- Keep ownership and lifetime visible at the point of use.
- Remove dead code and duplicated logic.
- Do not compress logic into a dense expression to reduce the line count.
- When existing code is larger than necessary, propose a specific reduction.
- Use empty lines to separate distinct parts of logic.
- Keep related statements together.
- Do not use repeated empty lines for decoration.

## C++

Use C++23. The code must work with the latest Apple Clang, GCC, and Microsoft
Visual C++ (MSVC) compilers.

## Review

Before completing a change, check:

- Is every new part required now?
- Can any helper, wrapper, or layer be removed?
- Are names consistent with the terminology dictionary?
- Does whitespace show the structure of the logic?
- Do the supported compilers accept every selected feature?
