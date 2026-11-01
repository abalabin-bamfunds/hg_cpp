#pragma once

#include <hg/details/evaluation_context.hpp>
#include <hg/export.hpp>
#include <hg/time_series/ref/ref_storage.hpp>

#include <span>

namespace hg::details {

extern HG_EXPORT const char inherited_time_series_section_type;

struct InheritedTimeSeriesSection : EvaluationContextSection {
    explicit InheritedTimeSeriesSection(std::span<const Reference> references) noexcept
        : EvaluationContextSection{nullptr, &inherited_time_series_section_type}, references{references} {}

    std::span<const Reference> references;
};

} // namespace hg::details
