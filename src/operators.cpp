#include <bg/operators.hpp>

#include <bg/node/node_template.hpp>
#include <bg/time_series/tsb/operators.hpp>
#include <bg/time_series/tsd/operators.hpp>
#include <bg/time_series/tsl/operators.hpp>

namespace bg::operators {

namespace {

class ConstantNodeTemplate final : public NodeTemplate {
  public:
    const Signature &signature() const override {
        static const auto signature = Signature{
            util::symbol{"const"}, {{util::symbol{"value"}, &SCALAR::type()}},
            TS<SCALAR, TimeSeriesGuarantee::const_>::type()};
        return signature;
    }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    std::optional<ConstantOutput> constant_output(const ResolvedSignature &) const noexcept override {
        return ConstantOutput{0};
    }
};

} // namespace

operator_<auto(SCALAR) -> TS<SCALAR, TimeSeriesGuarantee::const_>> const_{"const", {"value"}};
operator_<auto(COLLECTION, Union<K, TS<K>>) -> ITEM> getitem{"getitem", {"collection", "key"}};
operator_<auto(TSB<SCHEMA>, util::symbol) -> ITEM> getattr{"getattr", {"bundle", "name"}};

namespace {

const auto registered = [] {
    const_.overload(ConstantNodeTemplate{});
    details::register_time_series_list_operators(getitem);
    details::register_time_series_dict_operators(getitem);
    details::register_time_series_bundle_operators(getattr);
    return true;
}();

} // namespace
} // namespace bg::operators
