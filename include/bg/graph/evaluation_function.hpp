#pragma once

namespace bg {

class CompiledNode;
class EvaluationContext;

using NodeFunction = void (*)(const CompiledNode &, void *, EvaluationContext *);

// Returns true when the node completed evaluation. A false result pauses the containing runtime graph.
using EvaluationFunction = bool (*)(const CompiledNode &, void *, EvaluationContext *);

} // namespace bg
