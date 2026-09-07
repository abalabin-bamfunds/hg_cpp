#pragma once

#include <bg/graph/node.hpp>
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
    std::size_t parameter_index() const noexcept { return parameter_index_; }
    util::symbol parameter_name() const noexcept { return metadata().name; }
    const Type &parameter_type() const noexcept { return *metadata().type; }

  protected:
    NodeInvocationParameter(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index,
                            std::size_t view_index) noexcept
        : node_{&node}, runtime_storage_{runtime_storage}, parameter_index_{parameter_index}, view_index_{view_index} {}

    const ParameterBinding &binding() const noexcept { return node_->parameters()[parameter_index_]; }

    const NodeInvocationParameterMetadata &metadata() const noexcept {
        return node_->node_invocation_metadata()->parameters(*node_)[parameter_index_];
    }

    const CompiledNode *node_;
    std::byte *runtime_storage_;
    std::size_t parameter_index_;
    std::size_t view_index_;
};

class NodeInvocationInput final : public NodeInvocationParameter {
  public:
    NodeInvocationInput(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index,
                        std::size_t view_index) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index, view_index} {}

    In<TS_T> value() const {
        return Node{*node_, runtime_storage_}.time_series_input(parameter_index_, view_index_);
    }
};

class NodeInvocationScalar final : public NodeInvocationParameter {
  public:
    NodeInvocationScalar(const CompiledNode &node, std::byte *runtime_storage, std::size_t parameter_index,
                         std::size_t view_index, std::size_t time_series_input_count) noexcept
        : NodeInvocationParameter{node, runtime_storage, parameter_index, view_index},
          time_series_input_count_{time_series_input_count} {}

    bool has_value() const noexcept { return binding().scalar_data() != nullptr; }

    Scalar<> value() const {
        return Scalar<>::from(node_->scalar_input_runtime_type(time_series_input_count_, view_index_),
                              binding().scalar_data());
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

        Iterator(const CompiledNode &node, std::byte *runtime_storage, std::size_t time_series_input_count,
                 std::size_t parameter_index, std::size_t view_index) noexcept
            : node_{&node}, runtime_storage_{runtime_storage}, time_series_input_count_{time_series_input_count},
              parameter_index_{parameter_index}, view_index_{view_index} {
            advance_to_category();
        }

        value_type operator*() const noexcept {
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
        std::size_t time_series_input_count_;
        std::size_t parameter_index_;
        std::size_t view_index_;
    };

    Iterator begin() const noexcept {
        return Iterator{*node_, runtime_storage_, time_series_input_count_, 0, 0};
    }
    std::default_sentinel_t end() const noexcept { return {}; }

  private:
    template <typename, typename> friend class NodeInvocation;

    NodeInvocationParameterRange(const CompiledNode &node, std::byte *runtime_storage,
                                 std::size_t time_series_input_count) noexcept
        : node_{&node}, runtime_storage_{runtime_storage}, time_series_input_count_{time_series_input_count} {}

    const CompiledNode *node_;
    std::byte *runtime_storage_;
    std::size_t time_series_input_count_;
};

template <typename CompiledData = void, typename State = void> class NodeInvocation {
  public:
    NodeInvocation(const CompiledNode &node, void *runtime_storage, EvaluationContext &context) noexcept
        : node_{&node}, runtime_storage_{static_cast<std::byte *>(runtime_storage)}, context_{&context} {}

    EvaluationContext &context() const noexcept { return *context_; }

    auto inputs() const noexcept -> NodeInvocationParameterRange<true> {
        return {*node_, runtime_storage_, 0};
    }

    auto scalars() const noexcept -> NodeInvocationParameterRange<false> {
        return {*node_, runtime_storage_, node_->node_invocation_time_series_input_count()};
    }

    Out<TS_T> output() const {
        const auto runtime_node = Node{*node_, runtime_storage_};
        return details::output_factory::make<TS_T>(runtime_node.output_last_updated(), runtime_node.output_value(),
                                                   node_->node_invocation_output_runtime_type(), *context_);
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
