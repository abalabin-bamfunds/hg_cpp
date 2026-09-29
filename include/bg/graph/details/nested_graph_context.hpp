#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/time_series/ref/ref_input.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace bg::details {

// Builds the child evaluation context containing an inner graph's inherited time series references.
class NestedGraphStartupContext {
  public:
    template <typename Invocation>
    NestedGraphStartupContext(Invocation invocation, std::size_t first_parameter, std::size_t count)
        : NestedGraphStartupContext{invocation, invocation.context(), first_parameter, count} {}

    template <typename Invocation>
    NestedGraphStartupContext(Invocation invocation, EvaluationContext &parent, std::size_t first_parameter,
                              std::size_t count)
        : parent_{&parent}, count_{count},
          references_{count == 0 ? nullptr
                                 : static_cast<Reference *>(
                                       parent_->scratch().allocate(count * sizeof(Reference), alignof(Reference)))},
          section_{{references_, count}}, context_{parent.child(section_)} {
        if (count == 0) {
            return;
        }

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
    }

    ~NestedGraphStartupContext() {
        if (references_ != nullptr) {
            parent_->scratch().release_last(references_, count_ * sizeof(Reference));
        }
    }

    NestedGraphStartupContext(const NestedGraphStartupContext &) = delete;
    NestedGraphStartupContext &operator=(const NestedGraphStartupContext &) = delete;

    EvaluationContext &context() noexcept { return references_ == nullptr ? *parent_ : context_; }

  private:
    EvaluationContext *parent_;
    std::size_t count_;
    Reference *references_;
    InheritedTimeSeriesSection section_;
    EvaluationContext context_;
};

} // namespace bg::details
