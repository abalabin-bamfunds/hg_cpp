#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/value.hpp>

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace bg {

class NodeInvocationParameter {
  public:
    [[nodiscard]] std::size_t parameter_index() const noexcept { return parameter_index_; }
    [[nodiscard]] util::symbol parameter_name() const noexcept { return metadata().name; }
    [[nodiscard]] const Type &parameter_type() const noexcept { return *metadata().type; }

  protected:
    NodeInvocationParameter(const CompiledNode &node, const std::byte *runtime_storage, std::size_t parameter_index,
                            std::size_t view_index) noexcept
        : node_{&node}, runtime_storage_{runtime_storage}, parameter_index_{parameter_index}, view_index_{view_index} {}

    [[nodiscard]] const ParameterBinding &binding() const noexcept { return node_->parameters()[parameter_index_]; }

    [[nodiscard]] const NodeInvocationParameterMetadata &metadata() const noexcept {
        return node_->node_invocation_metadata()->parameters(*node_)[parameter_index_];
    }

    const CompiledNode *node_;
    const std::byte *runtime_storage_;
    std::size_t parameter_index_;
    std::size_t view_index_;
};

class NodeInvocationInput final : public NodeInvocationParameter {
  public:
    NodeInvocationInput(const CompiledNode &node, const std::byte *runtime_storage, std::size_t parameter_index,
                        std::size_t view_index) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index, view_index} {}

    [[nodiscard]] In<TS_T> value() const {
        const auto &prototype = [&]() -> const TimeSeriesInputView & {
            if (binding().mode() == ParameterBindingMode::reference) {
                const auto *target = binding().reference_input(runtime_storage_).target;
                if (target != nullptr) {
                    return target->target_representation->input_view_prototype();
                }
            }
            return node_->time_series_input_view_prototype(view_index_);
        }();
        return In<TS_T>{binding().last_updated(runtime_storage_), binding().value(runtime_storage_), prototype};
    }
};

class NodeInvocationScalar final : public NodeInvocationParameter {
  public:
    NodeInvocationScalar(const CompiledNode &node, const std::byte *runtime_storage, std::size_t parameter_index,
                         std::size_t view_index, std::size_t time_series_input_count) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index, view_index},
          time_series_input_count_{time_series_input_count} {}

    [[nodiscard]] bool has_value() const noexcept { return binding().scalar_data() != nullptr; }

    [[nodiscard]] Scalar<> value() const {
        const auto &prototype = static_cast<const ScalarValueInputView &>(
            node_->scalar_input_view_prototype(time_series_input_count_, view_index_));
        return Scalar<>::from(prototype, binding().scalar_data());
    }

  private:
    std::size_t time_series_input_count_;
};

template <bool TimeSeries> class NodeInvocationParameterRange {
  public:
    using value_type = std::conditional_t<TimeSeries, NodeInvocationInput, NodeInvocationScalar>;

    class Iterator {
      public:
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;

        Iterator(const CompiledNode &node, const std::byte *runtime_storage, std::size_t time_series_input_count,
                 std::size_t parameter_index, std::size_t view_index) noexcept
            : node_{&node}, runtime_storage_{runtime_storage}, time_series_input_count_{time_series_input_count},
              parameter_index_{parameter_index}, view_index_{view_index} {
            advance_to_category();
        }

        [[nodiscard]] value_type operator*() const noexcept {
            if constexpr (TimeSeries) {
                return {*node_, runtime_storage_, parameter_index_, view_index_};
            } else {
                return {*node_, runtime_storage_, parameter_index_, view_index_, time_series_input_count_};
            }
        }

        Iterator &operator++() noexcept {
            ++parameter_index_;
            ++view_index_;
            advance_to_category();
            return *this;
        }

        [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept {
            return parameter_index_ == node_->parameters().size();
        }

      private:
        void advance_to_category() noexcept {
            const auto parameters = node_->node_invocation_metadata()->parameters(*node_);
            while (parameter_index_ < parameters.size() &&
                   parameters[parameter_index_].type->is_time_series() != TimeSeries) {
                ++parameter_index_;
            }
        }

        const CompiledNode *node_;
        const std::byte *runtime_storage_;
        std::size_t time_series_input_count_;
        std::size_t parameter_index_;
        std::size_t view_index_;
    };

    [[nodiscard]] Iterator begin() const noexcept {
        return Iterator{*node_, runtime_storage_, time_series_input_count_, 0, 0};
    }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

  private:
    template <typename, typename> friend class NodeInvocation;

    NodeInvocationParameterRange(const CompiledNode &node, const std::byte *runtime_storage,
                                 std::size_t time_series_input_count) noexcept
        : node_{&node}, runtime_storage_{runtime_storage}, time_series_input_count_{time_series_input_count} {}

    const CompiledNode *node_;
    const std::byte *runtime_storage_;
    std::size_t time_series_input_count_;
};

template <typename CompiledData = void, typename State = void> class NodeInvocation {
  public:
    NodeInvocation(const CompiledNode &node, void *runtime_storage, EvaluationContext &context) noexcept
        : node_{&node}, runtime_storage_{static_cast<std::byte *>(runtime_storage)}, context_{&context} {}

    [[nodiscard]] EvaluationContext &context() const noexcept { return *context_; }

    [[nodiscard]] auto inputs() const noexcept -> NodeInvocationParameterRange<true> {
        return {*node_, runtime_storage_, 0};
    }

    [[nodiscard]] auto scalars() const noexcept -> NodeInvocationParameterRange<false> {
        return {*node_, runtime_storage_, node_->node_invocation_time_series_input_count()};
    }

    [[nodiscard]] Out<TS_T> output() const {
        return details::output_factory::make<TS_T>(node_->output_last_updated(runtime_storage_),
                                                   runtime_storage_ + node_->output_value_offset(),
                                                   node_->node_invocation_output_view_prototype(), *context_);
    }

    template <typename Value = CompiledData>
        requires(!std::is_void_v<Value>)
    [[nodiscard]] const Value &compiled_data() const noexcept {
        return *static_cast<const Value *>(node_->node_invocation_metadata()->compiled_data(*node_));
    }

    template <typename Value = State>
        requires(!std::is_void_v<Value>)
    [[nodiscard]] Value &state() const noexcept {
        return *reinterpret_cast<Value *>(runtime_storage_ + node_->state_offset());
    }

  private:
    const CompiledNode *node_;
    std::byte *runtime_storage_;
    EvaluationContext *context_;
};

} // namespace bg
