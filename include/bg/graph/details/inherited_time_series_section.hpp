#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/export.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <span>

namespace bg::details {

extern BG_EXPORT const char inherited_time_series_section_type;

struct InheritedTimeSeriesSection : EvaluationContextSection {
    explicit InheritedTimeSeriesSection(std::span<const Reference> references) noexcept
        : EvaluationContextSection{nullptr, &inherited_time_series_section_type}, references{references} {}

    std::span<const Reference> references;
};

} // namespace bg::details
