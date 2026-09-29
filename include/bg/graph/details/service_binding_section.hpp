#pragma once

#include <bg/details/evaluation_context.hpp>

#include <span>

namespace bg::details {

inline constexpr unsigned char service_binding_section_type{};

struct ServiceBindingSection : EvaluationContextSection {
    explicit ServiceBindingSection(std::span<void *const> bindings) noexcept
        : EvaluationContextSection{nullptr, &service_binding_section_type}, bindings{bindings} {}

    std::span<void *const> bindings;
};

} // namespace bg::details
