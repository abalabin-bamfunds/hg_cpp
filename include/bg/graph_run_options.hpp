#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/global_state.hpp>
#include <bg/service_catalogue.hpp>

#include <optional>

namespace bg {

struct GraphRunOptions {
    GlobalState *global_state{};
    const ServiceCatalogue *services{};
    std::optional<EvaluationProfiler::Recorder> recorder{};
};

} // namespace bg
