#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/testing/test_wiring.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace wiring_point_methods_test {

template <template <typename, auto...> typename Field> struct scalar_bundle {
    Field<hg::i64> value;
};

template <template <typename, auto...> typename Field> struct time_series_bundle {
    Field<hg::TS<hg::i64>> value;
};

using scalar_bundle_type = hg::TS<hg::Bundle<scalar_bundle>>;
using time_series_bundle_type = hg::TSB<time_series_bundle>;

} // namespace wiring_point_methods_test

namespace hg {

template <auto... Properties>
struct wiring_point_methods<wiring_point_methods_test::scalar_bundle_type,
                            wp<wiring_point_methods_test::scalar_bundle_type, Properties...>> {
    using point_type = wp<wiring_point_methods_test::scalar_bundle_type, Properties...>;

    auto value() const {
        return operators::getattr(static_cast<const point_type &>(*this), util::symbol{"value"});
    }
};

template <auto... Properties>
struct wiring_point_methods<wiring_point_methods_test::time_series_bundle_type,
                            wp<wiring_point_methods_test::time_series_bundle_type, Properties...>> {
    using point_type = wp<wiring_point_methods_test::time_series_bundle_type, Properties...>;

    auto value() const { return static_cast<const point_type &>(*this)->value; }
};

} // namespace hg

TEST_CASE("generic wiring point method extensions wire operators",
          "[feature:wiring][feature:getattr][series:value][scalar:hierarchical][schema:named]") {
    using point_type = hg::wp<wiring_point_methods_test::scalar_bundle_type>;

    static_assert(std::is_empty_v<hg::wiring_point_methods<wiring_point_methods_test::scalar_bundle_type,
                                                           point_type>>);
    static_assert(std::is_trivially_copyable_v<hg::wiring_point_methods<wiring_point_methods_test::scalar_bundle_type,
                                                                      point_type>>);
    static_assert(sizeof(point_type) == sizeof(hg::WiringPoint));
    static_assert(std::is_trivially_copyable_v<point_type>);
    static_assert(std::is_convertible_v<point_type, hg::WiringPoint>);

    const auto read_value = hg::graph([](point_type point) { return point.value(); });
    const auto output = hg::run_graph(
        read_value, hg::input<wiring_point_methods_test::scalar_bundle_type>({{{"value", 3}}, {{"value", 7}}}));
    CHECK(output.equals<hg::i64>({3, 7}));
}

TEST_CASE("typed wiring point method extensions preserve annotations and representation",
          "[feature:wiring][feature:getattr][series:tsb][schema:named][validity:optional]") {
    using bundle_type = wiring_point_methods_test::time_series_bundle_type;
    using point_type = hg::wp<bundle_type>;
    using optional_point_type = hg::wp<bundle_type, hg::optional>;
    using variadic_bundle_point = hg::wp<bundle_type, hg::var_kwargs>;
    using variadic_list_point = hg::wp<hg::TSL<hg::TS<hg::i64>>, hg::var_args>;

    static_assert(std::is_empty_v<hg::wiring_point_methods<bundle_type, point_type>>);
    static_assert(std::is_trivially_copyable_v<hg::wiring_point_methods<bundle_type, point_type>>);
    static_assert(sizeof(point_type) == sizeof(hg::WiringPoint));
    static_assert(sizeof(optional_point_type) == sizeof(hg::WiringPoint));
    static_assert(std::is_trivially_copyable_v<point_type>);
    static_assert(std::is_trivially_copyable_v<optional_point_type>);
    static_assert(std::is_convertible_v<point_type, hg::WiringPoint>);
    static_assert(std::is_convertible_v<optional_point_type, hg::WiringPoint>);
    static_assert(sizeof(variadic_bundle_point) == sizeof(hg::WiringPoint));
    static_assert(std::is_trivially_copyable_v<variadic_bundle_point>);
    static_assert(sizeof(variadic_list_point) == sizeof(hg::WiringPoint));
    static_assert(std::is_trivially_copyable_v<variadic_list_point>);

    const auto read_value = hg::graph([](point_type point) { return point.value(); });
    const auto output = hg::run_graph(
        read_value, hg::input<bundle_type>({{{"value", 2}}, {{"value", 9}}}));
    CHECK(output.equals<hg::i64>({2, 9}));

    const auto source = hg::node([](hg::Out<bundle_type>) {});
    hg::testing::test_wiring([&] {
        const auto optional_point = hg::opt(source());
        const auto field = optional_point.value();
        CHECK(&field.type() == &hg::TS<hg::i64>::type());
    });
}
