#pragma once

#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>

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

using namespace bg;
using namespace bg::literals;
using namespace bg::util::literals;

template <template <typename, auto...> typename Field> struct ServiceKey {
    Field<i64> tenant;
    Field<symbol> name;
};

struct ServiceOutputSource {
    Out<TS<std::int64_t>> output;

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
