#pragma once

namespace bg {

class CompiledNode;
class EvaluationContext;

using EvaluationFunction = void (*)(const CompiledNode &, void *, EvaluationContext *);

} // namespace bg
