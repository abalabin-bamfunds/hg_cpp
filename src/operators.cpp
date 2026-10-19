#include <bg/operators.hpp>

#include <bg/node/node_template.hpp>
#include <bg/time_series/tsb/operators.hpp>
#include <bg/time_series/tsd/operators.hpp>
#include <bg/time_series/tsl/operators.hpp>

#include <array>
#include <optional>
#include <span>

namespace bg::operators {

namespace {

class ConstantNodeTemplate final : public NodeTemplate {
  public:
    const Signature &default_signature() const override {
        static const auto signature = Signature{util::symbol{"const"},
                                                {{util::symbol{"value"}, &SCALAR::type()}},
                                                TS<SCALAR, TimeSeriesGuarantee::const_>::type()};
        return signature;
    }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    std::optional<ConstantOutput> constant_output(const ResolvedSignature &) const noexcept override {
        return ConstantOutput{0};
    }
};

class DefaultInputNodeTemplate final : public NodeTemplate {
  public:
    const Signature &default_signature() const override {
        static const auto signature =
            Signature{util::symbol{"default input"},
                      {{util::symbol{"value"}, &TS<SCALAR>::type(), active, optional},
                       {util::symbol{"fallback"}, &TS<SCALAR, TimeSeriesGuarantee::const_>::type()}},
                      TS<SCALAR>::type()};
        return signature;
    }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    std::optional<DefaultOutput> default_output(const ResolvedSignature &) const noexcept override {
        return DefaultOutput{0, 1};
    }
};

const auto default_input_node = DefaultInputNodeTemplate{};

class DefaultValueTemplate final : public WiringTemplate {
  public:
    const Signature &default_signature() const override {
        static const auto signature =
            Signature{util::symbol{"default"},
                      {{util::symbol{"value"}, &TS<SCALAR>::type(), active, optional},
                       {util::symbol{"fallback"}, &SCALAR::type()}},
                      TS<SCALAR>::type()};
        return signature;
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const override {
        const auto fallback = std::array{arguments[1]};
        const auto result = static_cast<const WiringTemplate &>(const_)(std::span<const WiringArgument>{fallback});
        const auto default_arguments = std::array{arguments[0], WiringArgument{*result}};
        return default_input_node(default_arguments);
    }
};

} // namespace

operator_<auto(SCALAR)->TS<SCALAR, TimeSeriesGuarantee::const_>> const_{"const", {"value"}};
operator_<auto(TS<SCALAR>, SCALAR)->TS<SCALAR>> default_{"default", {"value", "fallback"}};
operator_<ITEM, auto(COLLECTION, Union<"key", K, TS<K>>)->ITEM> getitem{"getitem", {"collection", "key"}};
operator_<ITEM, auto(TS_T, SCALAR)->ITEM> getattr{"getattr", {"bundle", "name"}};

namespace {

const auto registered = [] {
    const_.overload(ConstantNodeTemplate{});
    default_.overload(DefaultValueTemplate{});
    details::register_time_series_list_operators(getitem);
    details::register_time_series_dict_operators(getitem);
    details::register_time_series_bundle_operators(getattr);
    return true;
}();

} // namespace
} // namespace bg::operators
