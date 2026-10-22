#pragma once

#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>

#include <optional>
#include <vector>

namespace bg::details {

struct OperatorResolutionMatch {
    const WiringTemplate *implementation;
    SignatureResolution resolution;
    std::vector<const TimeSeriesType *> argument_forms;
    std::vector<std::optional<std::size_t>> supplied_argument_indices;
};

struct OperatorResolutionResult {
    std::optional<OperatorResolutionMatch> selected;
    std::vector<WiringCandidateDiagnostic> failures;
    bool ambiguous{};
};

} // namespace bg::details
