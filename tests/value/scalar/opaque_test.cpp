#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory_resource>

namespace {

struct private_data {
    explicit private_data(std::array<int, 4> source) : values(source) {}

    std::array<int, 4> values;

    private_data(const private_data &) = default;
    private_data &operator=(const private_data &) = delete;
};

using PrivateDataValue = bg::ScalarValue<bg::Opaque<private_data>>;
using PrivatePointerValue = bg::ScalarValue<bg::Opaque<private_data *>>;

} // namespace

TEST_CASE("an opaque scalar value owns a copy", "[value][scalar][opaque]") {
    auto source = private_data{{1, 2, 3, 4}};
    const auto value = PrivateDataValue{source};

    source.values[0] = 5;

    CHECK(value.view().value().values == std::array{1, 2, 3, 4});
    CHECK(&value.view().type() == &bg::cpp_scalar_type<bg::Opaque<private_data>>());
}

TEST_CASE("a pointer-sized opaque scalar value uses inline storage", "[value][scalar][opaque]") {
    auto source = private_data{{1, 2, 3, 4}};
    const auto value = PrivatePointerValue{&source, *std::pmr::null_memory_resource()};

    CHECK(value.view().value() == &source);
}
