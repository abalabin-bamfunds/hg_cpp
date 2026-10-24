#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/periodic_callback.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/global_state.hpp>
#include <bg/service_catalogue.hpp>

#include <optional>
#include <stop_token>
#include <stdexcept>

namespace bg {

struct GraphRunOptions {
    GlobalState *global_state{};
    const ServiceCatalogue *services{};
    std::optional<EvaluationProfiler::Recorder> recorder{};
    EvaluationEngine engine{EvaluationEngine::simulation};
    bool capture_output{true};
    std::optional<EvaluationTime> start_time{};
    std::optional<EvaluationTime> stop_time{};
    std::optional<EvaluationTime::duration> run_time{};
    std::stop_token stop_token{};
    PeriodicCallback periodic_callback{};

    [[nodiscard]] std::optional<EvaluationTime> resolve_stop_time(EvaluationTime resolved_start_time) const {
        auto resolved_stop_time = stop_time;
        if (resolved_stop_time && *resolved_stop_time <= resolved_start_time) {
            throw std::invalid_argument{"stop_time must be later than start_time"};
        }

        if (!run_time) {
            return resolved_stop_time;
        }
        if (*run_time <= EvaluationTime::duration::zero()) {
            throw std::invalid_argument{"run_time must be positive"};
        }

        if (resolved_start_time > EvaluationTime::max() - *run_time) {
            throw std::overflow_error{"run_time overflows stop_time"};
        }

        const auto run_stop_time = resolved_start_time + *run_time;
        if (!resolved_stop_time || run_stop_time < *resolved_stop_time) {
            resolved_stop_time = run_stop_time;
        }
        return resolved_stop_time;
    }
};

} // namespace bg
