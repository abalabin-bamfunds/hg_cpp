#include <bg/graph.hpp>
#include <bg/library/operators.hpp>

#include <bg/node.hpp>
#include <bg/scalar/scalar.hpp>

namespace bg::operators {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

struct add_time_series {
    In<TS<double, G>> lhs;
    In<TS<double, G>> rhs;
    Out<TS<double, G>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

struct add_scalar_time_series {
    Scalar<double> lhs;
    In<TS<double, G>> rhs;
    Out<TS<double, G>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

struct add_time_series_scalar {
    In<TS<double, G>> lhs;
    Scalar<double> rhs;
    Out<TS<double, G>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

const auto add_scalars = graph("name"_n = "add_scalars", [](double lhs, double rhs) {
    return operators::const_(lhs + rhs).as<TS<double, TimeSeriesGuarantee::const_>>();
});

} // namespace

decltype(add) add{"add", {"lhs", "rhs"}};

namespace {

const auto registered = [] {
    add.overload(add_scalars);
    add.overload(node<add_scalar_time_series>());
    add.overload(node<add_time_series_scalar>());
    add.overload(node<add_time_series>());
    return true;
}();

} // namespace
} // namespace bg::operators
