#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/value.hpp>
#include <bg/scalar/string_builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string_view>

using namespace bg;

namespace {

void count_final_release(void *context, const details::StringStorage *) noexcept {
    ++*static_cast<int *>(context);
}

} // namespace

TEST_CASE("a stored string view retains its canonical storage",
          "[feature:scalar-storage][scalar:string][storage:variable]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    const auto &runtime_type = cpp_scalar_runtime_type<string>();
    const auto source = std::string_view{"stored string"};
    auto *stored = allocator.allocate_bytes(runtime_type.size(), runtime_type.alignment());
    runtime_type.construct_from_input(
        stored, details::ScalarData{runtime_type, &source, ScalarDataMode::source_view}, allocator);

    const auto *original = *static_cast<details::StringStorage **>(stored);
    const auto view = Scalar<string>::from(*static_cast<details::StringStorage **>(stored));
    auto copy = ScalarValue<string>{view};

    CHECK(details::StringStorage::from(details::scalar_data(copy.view())) == original);
    CHECK(copy.view().value() == source);

    runtime_type.destroy(stored, allocator);
    allocator.deallocate_bytes(stored, runtime_type.size(), runtime_type.alignment());
    CHECK(copy.view().value() == source);
}

TEST_CASE("a string storage hook runs after its last retained value",
          "[feature:scalar-storage][scalar:string][storage:variable]") {
    auto release_count = 0;
    auto hook = details::StringStorageFinalReleaseHook{count_final_release, &release_count};

    {
        auto value = ScalarValue<string>{"shared string"};
        auto *storage = details::StringStorage::from(details::scalar_data(value.view()));
        REQUIRE(storage != nullptr);
        CHECK(storage->install_final_release_hook(&hook) == &hook);
        CHECK(storage->final_release_hook() == &hook);

        {
            auto copy = ScalarValue<string>{value.view()};
            CHECK(details::StringStorage::from(details::scalar_data(copy.view())) == storage);
        }
        CHECK(release_count == 0);
    }

    CHECK(release_count == 1);
}

TEST_CASE("a string builder grows and appends a view of its own value", "[coverage-exempt]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    {
        auto builder = StringBuilder{allocator, 4};
        builder.append("abc");
        builder.append(builder.view());
        builder.append("defghijklmnopqrstuvwxyz");

        auto value = std::move(builder).finish();
        CHECK(value.view().value() == "abcabcdefghijklmnopqrstuvwxyz");
    }
}

TEST_CASE("a string builder rejects values larger than 4 GiB", "[coverage-exempt]") {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        auto builder = StringBuilder{};
        const auto too_large = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
        CHECK_THROWS_AS(builder.reserve(too_large), std::length_error);
    }
}
