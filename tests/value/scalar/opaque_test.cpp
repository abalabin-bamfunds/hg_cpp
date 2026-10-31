#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory_resource>

using namespace bg;

namespace {

struct private_data {
    explicit private_data(std::array<int, 4> source) : values(source) {}

    std::array<int, 4> values;

    private_data(const private_data &) = default;
    private_data &operator=(const private_data &) = delete;
};

} // namespace

TEST_CASE("an opaque scalar value owns a copy", "[value][scalar][opaque][scalar:opaque][feature:scalar-api]") {
    using PrivateDataValue = ScalarValue<Opaque<private_data>>;

    auto source = private_data{{1, 2, 3, 4}};
    const auto value = PrivateDataValue{source};

    source.values[0] = 5;

    const auto view = value.view();
    CHECK(view->values == std::array{1, 2, 3, 4});
    CHECK(&view.type() == &cpp_scalar_type<Opaque<private_data>>());
}

TEST_CASE("a pointer-sized opaque scalar value uses inline storage",
          "[value][scalar][opaque][scalar:opaque][feature:scalar-api][storage:inline]") {
    using PrivatePointerValue = ScalarValue<Opaque<private_data *>>;

    auto source = private_data{{1, 2, 3, 4}};
    auto allocator = bg::allocator{*std::pmr::null_memory_resource()};
    const auto value = PrivatePointerValue{&source, allocator};

    const auto view = value.view();
    CHECK(*view == &source);
    CHECK(view->values == std::array{1, 2, 3, 4});
}
