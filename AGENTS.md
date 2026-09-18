# Project instructions

These instructions apply to the complete repository.

If there is a LOCAL_AGENTS.md in the directory, read and apply it too.

## Project direction

[docs/project.md](docs/project.md) defines the project purpose and priorities.
Use its priority order when design goals conflict.

[docs/node-layout.md](docs/node-layout.md) contains the current node object
layout proposal. Do not treat a proposed choice as an approved design.

[docs/node-api.md](docs/node-api.md) defines node inputs, evaluation functions,
and output modification.

[docs/graph-wiring.md](docs/graph-wiring.md) defines graph functions, wiring
templates, graph templates, wiring contexts, and wiring paths.

[docs/variadic-wiring.md](docs/variadic-wiring.md) contains the accepted
variadic wiring design. It is not implemented.

[docs/operators.md](docs/operators.md) defines operators, overload resolution,
requirements, resolution caching, and overload diagnostics.

[docs/evaluation.md](docs/evaluation.md) defines evaluation order, context
services, lifecycle, scheduling, cycle callbacks, and failure behavior.

[docs/type-system.md](docs/type-system.md) defines type resolution, time series
guarantees, compatibility, propagation, and runtime enforcement.

[docs/scalar-api.md](docs/scalar-api.md) defines scalar access, ownership,
construction, and generic list access.

[docs/global-state.md](docs/global-state.md) defines scalar values shared by an
outer graph and its inner graphs.

[docs/nested-graphs.md](docs/nested-graphs.md) defines nested graph boundaries,
ownership, lifecycle, and scheduling.

[docs/testing.md](docs/testing.md) defines test organization and required
coverage.

[docs/time-series-references.md](docs/time-series-references.md) defines reference
routing, dynamic subscriptions, and removal lifetime.

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

## Decision records

  - Documentation describes current behavior; `docs/decisions/` records why significant choices were made.
  - Record only durable, non-obvious decisions implemented in committed code. Include the record in the same authorized commit.
  - Skip routine details, plans, experiments, and uncommitted work.
  - Link relevant documentation instead of duplicating it.
  - Preserve replaced decisions and mark them `superseded`.
  - Never store secrets, transcripts, chain-of-thought, or speculation.

## Code

Prefer the smallest clear implementation, the code should be elegant.

- Use existing utilities fromm this project and the standard library before adding project code or
  a dependency. Prefer utility code already in the project
- Write each operation in execution order and at one consistent level of
  detail. Use early returns and `continue` statements to keep nesting shallow.
- Make each branch represent required behavior or a state transition. Prefer
  one representation until another has a measured benefit.
- Name intermediate values by their role and access clear members directly.
  Avoid aliases, accessors, and wrappers that make readers follow more code.
- Extract a function or class only when it has a coherent responsibility and
  gives its caller a smaller, clearer interface. Keep simple operations at
  their point of use.
- Put an operation on the object that owns the state or invariant it uses. Use
  a free function only for a symmetric algorithm, factory, public customization
  point, or operation with no natural owner. Do not add namespace-scope helpers
  that inspect an owner's internals or only forward to it.
- Add defensive checks only when a value can be null or invalid by design.
- Do not add ways to replace behavior, special object-creation code, or settings
  for an unconfirmed need.
- Keep ownership and lifetime visible at the point of use.
- Use scope-bound cleanup for rollback. Create the guard after acquisition and release it at the commit point.
- Remove dead code and duplicated logic.
- Do not compress logic into a dense expression to reduce the line count.
- When existing code is larger than necessary, propose a specific reduction.
- Use empty lines to separate distinct parts of logic.
- Keep related statements together.
- Do not use repeated empty lines for decoration.
- Keep code within 120 columns unless a longer line is clearer.

## Comments

- Comment code when its purpose or contract is not clear from its interface.
- State what the code provides, what it returns, and any non-obvious behavior.
- Describe intent at the caller's level. Explain mechanics only when a
  constraint makes them important.
- Do not narrate statements that are already clear from the code.

## C++

Use C++23. The code must work with the latest Apple Clang, GCC, and Microsoft
Visual C++ (MSVC) compilers.

- Use `auto` by default. Spell out a type when it adds information that the
  initializer does not show or selects a required representation.
- Spell composite time series types directly in node and graph declarations.
  Do not hide types such as `TSD<K, TS<V>>` behind a C++ type alias there.
- Name graph type variables in uppercase, for example `KEY`, `VALUE`, and
  `ITEM`.
- Use `[[nodiscard]]` only when ignoring a result loses an operation outcome,
  ownership, a handle, or error information. Do not apply it to observation APIs.
- Do not use `dynamic_cast` in evaluation, notification, subscription,
  time-series access, or other graph runtime paths. Put runtime dispatch on the
  runtime type, visitor, tagged representation, or object that owns the
  behavior. `dynamic_cast` is acceptable during wiring, type resolution,
  diagnostics, and checked boundary validation.
- Prefer C++23 language and library features when all supported compilers
  implement them.
- Prefer ranges, views, spans, structured bindings, and designated initializers
  when they make data flow clearer.

## Review

Before completing a change, check:

- Is every new part required now?
- Can any helper, wrapper, or layer be removed?
- Does each new runtime downcast or namespace-scope helper indicate a missing
  abstraction or owner?
- Does any special-case logic indicate a missing abstraction or inconsistent
  representation? Prefer a solution that follows the normal design. Keep a
  special case only when a concrete constraint requires it. Raise it with the
  project owner before retaining it, and document the constraint.
- Are names consistent with the terminology dictionary?
- Does whitespace show the structure of the logic?
- Do the supported compilers accept every selected feature?
