#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/evaluation_interruption.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/global_state.hpp>
#include <bg/service_catalogue.hpp>

#include <optional>
#include <stop_token>

namespace bg {

struct GraphRunOptions {
    GlobalState *global_state{};
    const ServiceCatalogue *services{};
    std::optional<EvaluationProfiler::Recorder> recorder{};
    EvaluationEngine engine{EvaluationEngine::simulation};
    std::optional<EvaluationTime> start_time{};
    std::stop_token stop_token{};
    EvaluationInterruption interruption{};
};

} // namespace bg
