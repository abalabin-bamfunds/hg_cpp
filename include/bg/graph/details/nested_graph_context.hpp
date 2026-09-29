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
    NestedGraphStartupContext(Invocation invocation, const CompiledNestedGraph &compiled,
                              std::size_t first_parameter, std::size_t count)
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

        const auto slots = std::span<const std::uint32_t>{compiled.service_binding_slots};
        if (!slots.empty()) {
            const auto *parent_section = static_cast<const ServiceBindingSection *>(
                parent.find_section(&service_binding_section_type));
            if (parent_section == nullptr) {
                throw std::invalid_argument{"an inner graph received no service bindings"};
            }
            if (std::ranges::any_of(slots, [&](auto slot) { return slot >= parent_section->bindings.size(); })) {
                throw std::invalid_argument{"an inner graph service binding slot is out of range"};
            }

            binding_count_ = slots.size();
            bindings_ = static_cast<void **>(
                parent.scratch().allocate(slots.size() * sizeof(void *), alignof(void *)));
            for (auto index = std::size_t{}; index < slots.size(); ++index) {
                bindings_[index] = parent_section->bindings[slots[index]];
            }

            service_section_.emplace(std::span<void *const>{bindings_, slots.size()});
            service_context_.emplace(current_context().child(*service_section_));
        }
    }

    ~NestedGraphStartupContext() {
        if (bindings_ != nullptr) {
            parent_->scratch().release_last(bindings_, binding_count_ * sizeof(void *));
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
    void **bindings_{};
    std::size_t binding_count_{};
    std::optional<ServiceBindingSection> service_section_;
    std::optional<EvaluationContext> service_context_;
};

} // namespace bg::details
