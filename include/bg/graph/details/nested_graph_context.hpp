#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/graph/details/service_binding_section.hpp>
#include <bg/graph/nested_graph.hpp>
#include <bg/time_series/ref/ref_input.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

namespace bg::details {

// Builds the child evaluation context containing an inner graph's inherited time series references.
class NestedGraphStartupContext {
  public:
    template <typename Invocation>
    NestedGraphStartupContext(Invocation invocation, const CompiledNestedGraph &compiled, std::size_t first_parameter,
                              std::size_t count)
        : NestedGraphStartupContext{invocation, invocation.context(), compiled, first_parameter, count} {}

    template <typename Invocation>
    NestedGraphStartupContext(Invocation invocation, EvaluationContext &parent, const CompiledNestedGraph &compiled,
                              std::size_t first_parameter, std::size_t count)
        : parent_{&parent}, count_{count},
          references_{count == 0 ? nullptr
                                 : static_cast<Reference *>(
                                       parent_->scratch().allocate(count * sizeof(Reference), alignof(Reference)))},
          inherited_section_{{references_, count}} {
        if (count != 0) {
            auto input = invocation.inputs().begin();
            while ((*input).parameter_index() < first_parameter) {
                ++input;
            }

            for (auto index = std::size_t{}; index < count; ++index) {
                const auto value = (*input).value();
                const auto reference = *value.visit([](const In<REF<TS_T>> *item) { return item->value(); }, ddv::noop);
                std::construct_at(references_ + index, value.valid() ? reference : Reference{});
                ++input;
            }

            inherited_context_.emplace(parent.child(inherited_section_));
        }

        const auto parent_binding_indices = std::span<const std::uint32_t>{compiled.parent_service_binding_indices};
        if (!parent_binding_indices.empty()) {
            const auto *parent_section =
                static_cast<const ServiceBindingSection *>(parent.find_section(&service_binding_section_type));
            if (parent_section == nullptr) {
                throw std::invalid_argument{"an inner graph received no service bindings"};
            }
            if (std::ranges::any_of(parent_binding_indices,
                                    [&](auto index) { return index >= parent_section->bindings.size(); })) {
                throw std::invalid_argument{"an inner graph parent service binding index is out of range"};
            }

            binding_count_ = parent_binding_indices.size();
            bindings_ = static_cast<ServiceRequestBinding **>(parent.scratch().allocate(
                binding_count_ * sizeof(ServiceRequestBinding *), alignof(ServiceRequestBinding *)));
            for (auto index = std::size_t{}; index < binding_count_; ++index) {
                bindings_[index] = parent_section->bindings[parent_binding_indices[index]];
            }

            service_section_.emplace(std::span<ServiceRequestBinding *const>{bindings_, binding_count_});
            service_context_.emplace(current_context().child(*service_section_));
        }
    }

    ~NestedGraphStartupContext() {
        if (bindings_ != nullptr) {
            parent_->scratch().release_last(bindings_, binding_count_ * sizeof(ServiceRequestBinding *));
        }
        if (references_ != nullptr) {
            parent_->scratch().release_last(references_, count_ * sizeof(Reference));
        }
    }

    NestedGraphStartupContext(const NestedGraphStartupContext &) = delete;
    NestedGraphStartupContext &operator=(const NestedGraphStartupContext &) = delete;

    EvaluationContext &context() noexcept { return current_context(); }

  private:
    EvaluationContext &current_context() noexcept {
        if (service_context_) {
            return *service_context_;
        }
        if (inherited_context_) {
            return *inherited_context_;
        }
        return *parent_;
    }

    EvaluationContext *parent_;
    std::size_t count_;
    Reference *references_;
    InheritedTimeSeriesSection inherited_section_;
    std::optional<EvaluationContext> inherited_context_;
    ServiceRequestBinding **bindings_{};
    std::size_t binding_count_{};
    std::optional<ServiceBindingSection> service_section_;
    std::optional<EvaluationContext> service_context_;
};

} // namespace bg::details
