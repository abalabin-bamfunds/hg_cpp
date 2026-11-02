#pragma once

#include <hg/graph.hpp>
#include <hg/map.hpp>
#include <hg/mesh.hpp>
#include <hg/node.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/switch.hpp>
#include <hg/run_graph.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace {

using namespace hg;
using namespace hg::literals;
using namespace hg::util::literals;

template <template <typename, auto...> typename Field> struct ServiceKey {
    Field<i64> tenant;
    Field<symbol> name;
};

struct ServiceOutputSource {
    Out<TS<i64>> output;

    void start() const { output.set(42); }
    void evaluate() const noexcept {}
};

template <typename KEY> struct FinalKeySource {
    Scalar<KEY> value;
    Out<TS<KEY, TimeSeriesGuarantee::final>> output;

    void start() const { output.set_value(value); }
    void evaluate() const noexcept {}
};

} // namespace
