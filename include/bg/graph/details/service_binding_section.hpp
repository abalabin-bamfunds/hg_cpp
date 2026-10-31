#pragma once

#include <bg/details/evaluation_context.hpp>

#include <cstdint>
#include <limits>
#include <span>

namespace bg::details {

inline constexpr unsigned char service_binding_section_type{};
inline constexpr std::uint32_t no_service_binding_provider_node = std::numeric_limits<std::uint32_t>::max();

class ServiceRequestBinding;

struct ServiceBindingSection : EvaluationContextSection {
    explicit ServiceBindingSection(std::span<ServiceRequestBinding *const> bindings) noexcept
        : EvaluationContextSection{nullptr, &service_binding_section_type}, bindings{bindings} {}

    std::span<ServiceRequestBinding *const> bindings;
};

} // namespace bg::details
