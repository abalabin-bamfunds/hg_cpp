#pragma once

#include <bg/graph/node.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/value.hpp>

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace bg {

class NodeInvocationParameter {
  public:
    std::size_t parameter_index() const noexcept { return parameter_index_; }
    util::symbol parameter_name() const noexcept { return metadata().name; }
    const Type &parameter_type() const noexcept { return *metadata().type; }

  protected:
    NodeInvocationParameter(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index) noexcept
        : node_{&node}, runtime_storage_{runtime_storage}, parameter_index_{parameter_index} {}

    const ParameterBinding &binding() const noexcept { return node_->parameters()[parameter_index_]; }

    const NodeInvocationParameterMetadata &metadata() const noexcept {
        return node_->node_invocation_metadata()->parameters(*node_)[parameter_index_];
    }

    const CompiledNode *node_;
    std::byte *runtime_storage_;
    std::size_t parameter_index_;
};

class NodeInvocationInput final : public NodeInvocationParameter {
  public:
    NodeInvocationInput(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index} {}

    In<TS_T> value() const {
        return Node{*node_, runtime_storage_}.time_series_input(parameter_index_);
    }
};

class NodeInvocationScalar final : public NodeInvocationParameter {
  public:
    NodeInvocationScalar(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index} {}

    bool has_value() const noexcept { return binding().scalar_data() != nullptr; }

    Scalar<> value() const {
        return Scalar<>::from(node_->scalar_input_runtime_type(parameter_index_), binding().scalar_data());
    }

    template <typename T>
        requires(!scalar_declaration_traits<T>::generic && !details::hierarchical_scalar_declaration<T> &&
                 !details::scalar_storage_supports_alternative_layouts<T>)
    typename scalar_storage_traits<T>::value_type value() const {
        using Traits = scalar_storage_traits<T>;
        return Traits::read(*static_cast<const typename Traits::stored_type *>(binding().scalar_data()));
    }
};

template <bool TimeSeries> class NodeInvocationParameterRange {
  public:
    using value_type = std::conditional_t<TimeSeries, NodeInvocationInput, NodeInvocationScalar>;

    class Iterator {
      public:
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;

        Iterator(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index) noexcept
            : node_{&node}, runtime_storage_{runtime_storage}, parameter_index_{parameter_index} {
            advance_to_category();
        }

        value_type operator*() const noexcept {
            return {*node_, runtime_storage_, parameter_index_};
        }

        Iterator &operator++() noexcept {
            ++parameter_index_;
            advance_to_category();
            return *this;
        }

        bool operator==(std::default_sentinel_t) const noexcept {
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
        std::byte *runtime_storage_;
        std::size_t parameter_index_;
    };

    Iterator begin() const noexcept { return Iterator{*node_, runtime_storage_, 0}; }
    std::default_sentinel_t end() const noexcept { return {}; }

  private:
    template <typename, typename> friend class NodeInvocation;

    NodeInvocationParameterRange(const CompiledNode &node, std::byte *runtime_storage) noexcept
        : node_{&node}, runtime_storage_{runtime_storage} {}

    const CompiledNode *node_;
    std::byte *runtime_storage_;
};

template <typename CompiledData = void, typename State = void> class NodeInvocation {
  public:
    NodeInvocation(const CompiledNode &node, void *runtime_storage, EvaluationContext &context) noexcept
        : node_{&node}, runtime_storage_{static_cast<std::byte *>(runtime_storage)}, context_{&context} {}

    EvaluationContext &context() const noexcept { return *context_; }

    std::size_t parameter_count() const noexcept { return node_->parameters().size(); }

    bool parameter_is_time_series(std::size_t parameter_index) const noexcept {
        return node_->parameters()[parameter_index].mode() != ParameterBindingMode::scalar;
    }

    const void *scalar_data(std::size_t parameter_index) const noexcept {
        return node_->parameters()[parameter_index].scalar_data();
    }

    auto inputs() const noexcept -> NodeInvocationParameterRange<true> { return {*node_, runtime_storage_}; }

    auto scalars() const noexcept -> NodeInvocationParameterRange<false> { return {*node_, runtime_storage_}; }

    Out<TS_T> output() const {
        const auto runtime_node = Node{*node_, runtime_storage_};
        return details::output_factory::make<Out<TS_T>>(runtime_node.output_last_updated(),
                                                        runtime_node.output_value(),
                                                        node_->output_runtime_type(), *context_);
    }

    template <typename Input, std::size_t ControlledInputIndex = ::bg::ControlledInput::no_control>
    Input input(std::size_t parameter_index) const {
        const auto runtime_node = Node{*node_, runtime_storage_};
        auto *activity = static_cast<bool *>(nullptr);
        if constexpr (Input::dynamic_activity) {
            if constexpr (ControlledInputIndex == ::bg::ControlledInput::no_control) {
                const auto controlled_inputs = node_->controlled_inputs();
                if (controlled_inputs.empty()) {
                    throw std::logic_error{
                        "node invocation requests dynamic_activity for an input without dynamic_activity"};
                }
                const auto controlled_input_index =
                    node_->node_invocation_metadata()->controlled_input_indices(*node_)[parameter_index];
                if (controlled_input_index >= controlled_inputs.size()) {
                    throw std::logic_error{
                        "node invocation requests dynamic_activity for an input without dynamic_activity"};
                }
                activity = runtime_node.input_activity(controlled_input_index);
            } else {
                activity = runtime_node.input_activity(ControlledInputIndex);
            }
        }
        const auto input = runtime_node.input_data(parameter_index);
        const auto modified = input.last_updated->valid() && input.last_updated->time() == context_->evaluation_time();
        if constexpr (details::direct_time_series_input<Input> && !details::input_requires_runtime_type<Input>) {
            return Input{*input.last_updated, static_cast<const typename Input::storage_type *>(input.value), modified,
                         activity};
        } else {
            const auto data = TimeSeriesInputData{
                runtime_node.time_series_input_runtime_type(parameter_index), *input.last_updated,
                input.value, modified};
            return Input{data, activity};
        }
    }

    NodeInvocationScalar scalar(std::size_t parameter_index) const noexcept {
        return {*node_, runtime_storage_, parameter_index};
    }

    template <typename TimeSeries> Out<TimeSeries> output() const {
        return output_as<Out<TimeSeries>>();
    }

    template <typename Output> Output output_as() const {
        const auto runtime_node = Node{*node_, runtime_storage_};
        if constexpr (details::direct_time_series_output<Output> && !details::output_requires_runtime_type<Output>) {
            auto &value = *static_cast<typename Output::storage_type *>(runtime_node.output_value());
            return details::output_factory::make<Output>(
                runtime_node.output_last_updated(), value, *context_);
        } else {
            return details::output_factory::make<Output>(
                runtime_node.output_last_updated(), runtime_node.output_value(),
                node_->output_runtime_type(), *context_);
        }
    }

    void initialize_output_reference(details::RootReference &reference) const noexcept {
        reference.owner_graph = &context_->runtime_graph();
        reference.set_output_node(*node_);
        reference.referenced_runtime_type = &node_->output_runtime_type();
    }

    template <typename Value = CompiledData>
        requires(!std::is_void_v<Value>)
    const Value &compiled_data() const noexcept {
        return *static_cast<const Value *>(node_->node_invocation_metadata()->compiled_data(*node_));
    }

    template <typename Value = State>
        requires(!std::is_void_v<Value>)
    Value &state() const noexcept {
        return *static_cast<Value *>(Node{*node_, runtime_storage_}.state());
    }

  private:
    const CompiledNode *node_;
    std::byte *runtime_storage_;
    EvaluationContext *context_;
};

} // namespace bg
