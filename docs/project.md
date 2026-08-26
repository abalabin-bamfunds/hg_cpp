# Project definition

## Purpose

`bg` is an engine for forward propagation through computation graphs.

The engine defines how users describe nodes. Users construct a graph by
connecting nodes. These connections define the forward flow of data. The engine
evaluates the graph according to that flow.

## Priorities

The priorities are listed from highest to lowest:

1. Correct graph evaluation.
2. Execution performance and memory use.
3. Engine code that a reasonably senior C++ developer can read, understand, and
   maintain.
4. Simple interfaces for describing nodes and writing graph code.
5. Useful maintenance, troubleshooting, and debugging tools for users.

Use this order to resolve a conflict between priorities.

## Performance and memory

Performance and memory use are primary design constraints. Use optimization
techniques and memory reduction methods when they provide a clear benefit. Add
focused utility classes and functions when they support these methods.

## Python support

The first goal is a working C++ engine. Then support node descriptions and graph
code in Python to provide a shorter coding cycle.
