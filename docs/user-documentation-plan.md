# User documentation plan

Build a user guide for readers who know basic programming and are new to
computation graphs. Teach graph authoring through runnable examples before
introducing detailed input views and reference contracts.

## Agreed approach

- Begin with plain value types in graph and node examples.
- Explain changing values from the beginning. Introduce time series annotations
  when an example needs capabilities such as input state inspection.
- Introduce plain optional node parameters before detailed input views when
  the node only needs to handle an absent value.
- Provide matching Python and C++ examples for every lesson. Use synchronized
  language tabs and keep inputs and expected updates equivalent.
- Use a small example, expected output, and a concrete outcome for each lesson.
- Follow the learning chapters with API and library reference material.

Keep engine documentation available separately from the user guide.

## Learning sequence

| Chapter | Reader outcome |
| --- | --- |
| Meet BG | Understand its purpose and read a small computation graph. |
| Install BG | Install the Python package or build and link the C++ library. |
| Your first graph | Run a single calculation with plain value types. |
| Values that change | Understand current values, skipped and repeated updates, initial validity, and independently updating inputs. |
| Connect and reuse calculations | Supply fixed arguments and compose graph functions. Distinguish graph construction from evaluation. |
| Write custom nodes | Calculate with plain values and connect custom nodes. Test their behavior. |
| Handle missing values | Use plain optional parameters and understand required inputs and fallback behavior. |
| Inspect time series state | Introduce explicit annotations and input views for validity, modification state, and update time. |
| Control evaluation | Use active and passive inputs, validity constraints, and output publication. |
| Select updates | Use filtering, sampling, and deduplication. |
| Keep state | Use running calculations and previous values, then introduce custom node state. |
| Work with time | Use delays, timers, scheduling, and simulation. |
| Run complete graphs | Connect sources and sinks and use `run_graph`. |
| Work with structured data | Introduce bundles and collections. Distinguish scalar containers from collections of time series. |
| Compute for many items | Give `map` and `reduce` separate lessons, including item addition and removal. |
| Select computations | Introduce selection and `switch`, including changes in the selected case. |
| Advanced applications | Give references, generic code, services, context time series, and `mesh` separate chapters with prerequisites. |
| Diagnose and improve graphs | Read errors, isolate failures, and profile evaluation. |

Testing starts with the first example and continues through the chapters.
Use sensor readings through the early lessons. Each lesson introduces only the
syntax needed for its outcome and provides a small change for the reader to try.

## Reference and task guides

- How-to guides answer specific tasks without requiring the complete tutorial.
- API reference covers authoring functions, decorators, types, input and output
  access, execution, and testing.
- Library reference groups operations by purpose and supports lookup by name.
- Engine documentation covers architecture, storage, extension contracts, and
  implementation details.

Reference entries describe purpose, signatures, supported types, update and
invalid-input behavior, errors, and a minimal example. Reference pages own
complete contracts; tutorials introduce the behavior their examples need and
link to those contracts.

## Initial drafts

- [Meet BG](guide/index.md)
- [Install BG](guide/installation.md)
- [Your first graph](guide/getting-started.md)
- [Values that change](guide/changing-values.md)

Review these pages for the teaching approach before expanding the remaining
chapters. The initial examples use the current Python and C++ APIs. Python uses
a graph function; C++ tests a plain-parameter node directly. C++ graph functions require
wiring point annotations, so introduce that syntax when teaching composition.
Verify the source installation instructions in a fresh environment before
publishing.

## Documentation site

The site uses Sphinx, MyST Markdown, and the Furo theme. The root
[Read the Docs configuration](../.readthedocs.yaml) uses the same source and
configuration as local builds. See [documentation builds](README.md) for build,
preview, and page-authoring commands.

The published site includes learning chapters and reference pages. Engine
design documents and planning notes remain outside the site.

## Publication work to plan

Automate example execution, then decide how to generate reference signatures
and document version availability. Import the repository into Read the Docs
when it is ready to publish.
