#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/service_catalogue.hpp>

#include <optional>

namespace bg::testing {

struct TestGraphOptions {
    const ServiceCatalogue *services{};
    std::optional<EvaluationProfiler::Recorder> recorder{};
};

} // namespace bg::testing
